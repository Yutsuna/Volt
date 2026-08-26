# `volt repl` -- specification et plan d'implementation

Document de reference unique. Fusionne la vision produit (`REPL_IDEA.md`), l'analyse
de coloration echo (`PLAN_REPL.md`), et le plan d'implementation M4 (`PLAN_REPL_1.md`),
reconcilies avec l'etat reel du code (issue #120, merge).

---

## Table des matieres

1. [Etat des lieux](#1-etat-des-lieux)
2. [Architecture](#2-architecture)
3. [Ce qui est fait -- moteur sans tete](#3-ce-qui-est-fait----moteur-sans-tete)
4. [Ce qui reste -- phase 3 : affichage des valeurs et builtins](#4-ce-qui-reste----phase-3--affichage-des-valeurs-et-builtins)
5. [Ce qui reste -- phase 4 : TUI](#5-ce-qui-reste----phase-4--tui)
6. [Ce qui reste -- phase 5 : completion semantique](#6-ce-qui-reste----phase-5--completion-semantique)
7. [Vision UX](#7-vision-ux)
8. [Fichiers touches](#8-fichiers-touches)
9. [Verification -- portes sequentielles](#9-verification----portes-sequentielles)
10. [Reste a faire hors REPL](#10-reste-a-faire-hors-repl)

---

## 1. Etat des lieux

M0 a M3 sont termines (534/534 tests). `BackendJIT` compile et execute via LLVM ORC,
`volt run` emet en `PerUnit` par defaut, et `--watch` recharge a chaud en repointant
les slots `@volt.fn.<sym>`.

M4 est divise en phases. Voici l'etat exact, verifie contre le code et le fichier
de passation `.agents/PROGRESS-issue-120.md` :

| Phase | Contenu | Etat |
|---|---|---|
| Phase 1 : `EvalUnit` et boucle non interactive | `JitBackend::EvalUnit`, `Driver::AppendUnit`/`AnalyzeUnit`, `ReplEval::Evaluator`, `ReplCore::Classify`, `FReplCommand`, multi-ligne, `volt repl < script.vl` | **FAIT** |
| Phase 2 : portee de session | Table `Name -> {SemaTypeId, Symbol}`, `ExternalGlobals`, greffage AST synthetique, `_` = derniere valeur, latence plate a 800 lignes | **FAIT** |
| Phase 3 : affichage des valeurs et builtins | Coloration echo, `:type`, `:layout`, `:ir`, `:asm`, `:doc`, `:src`, `:bench`, generations ephemeres | **A FAIRE** |
| Phase 4 : TUI | `termios`, ANSI 24 bits, edition de ligne, panneau lateral, pager, `ReplTui` comme seul module I/O | **A FAIRE** |
| Phase 5 : completion semantique | `expr.`, identifiant nu, `:` builtin, ghost-text | **A FAIRE** |

Tests actuels : **549/549** (534 pre-REPL + 11 `repl/` + 4 ajouts correctif RAII).

### Limites connues, issues de la phase 2

Deux limites ont ete identifiees et documentees dans `PROGRESS-issue-120.md`. Elles
ne sont pas corrigees :

1. **Globals d'une ligne qui redefinit** : si une ligne redefinit une fonction *et*
   declare une variable neuve, cette variable vit dans la dylib laterale et est
   invisible depuis la principale.
2. **Redefinir une fonction de la stdlib** ne prend pas effet : le corps est dans
   le `.so`, sans slot. A refuser explicitement avec un diagnostic.

---

## 2. Architecture -- `source/Volt/REPL/`

Sept bibliotheques meson, sur le modele exact de `source/Volt/Driver/meson.build`
(`Public/Volt/<Mod>/` + `Private/`, sources globees, `configure_file(export_template)`,
`declare_dependency`). Le graphe est strictement acyclique et **`ReplTui` est le seul
module autorise a faire de l'I/O**.

```
ReplDoc/       vocabulaire partage : Span style, Document, Table, Pane.  <- Core seul
ReplSyntax/    Volt (via Frontend::Lexer) + LLVM IR + ASM -> spans.      <- ReplDoc, Frontend
ReplEval/      pipeline incremental Driver+JIT, une ligne = une unite.   <- Driver, BackendCore
ReplQuery/     builtins :type :layout :ir :asm :src :doc :bench.         <- ReplDoc, ReplEval
ReplComplete/  TypeStore/Registry/session -> candidats.                  <- ReplDoc, ReplEval
ReplCore/      etat de session, classification de ligne, historique.     <- tous les precedents
ReplTui/       termios, ANSI 24 bits, edition, split-pane, pager.        <- ReplCore, ReplDoc
```

Les six premiers rendent des structures de donnees. Un test de conformite (voir
porte 3, section 9) echoue si `<iostream>`, `std::cout`, `printf`, `std::cerr` ou
`::write` apparait ailleurs que dans `ReplTui/`.

`ReplDoc` est deliberement le socle : c'est le seul vocabulaire que `ReplQuery`,
`ReplSyntax` et `ReplComplete` partagent avec `ReplTui`, et le fait qu'il ne connaisse
ni terminal ni compilateur est ce qui rend les cinq autres testables sans TTY.

### Etat actuel de l'arborescence

Existent et sont complets :

- `source/Volt/REPL/meson.build` -- charge `ReplEval` et `ReplCore`.
- `source/Volt/REPL/Prelude/Repl.vl` -- `__volt_repl_echo<T>` (valeur via `.inspect`).
- `source/Volt/REPL/ReplEval/` -- `Evaluator` : possede `Driver` + `JitBackend`,
  `Feed(line)`, table de session (`SessionVar`), `Harvest`, `BindResult`,
  `NameForeignStorage`, `IsPrintable`, `DescribeType`.
- `source/Volt/REPL/ReplCore/` -- `Classify(Accumulated)` : bloc/bracket/continuation
  sur le flux de tokens du lexer natif.
- `source/Volt/Volt/.../ReplCommand.{hpp,cpp}` -- `FReplCommand`, mode interactif
  (prompt + multi-ligne) et mode pipe (`< script.vl`), `-e EXPR`, `-O LEVEL`.

N'existent pas encore : `ReplDoc/`, `ReplSyntax/`, `ReplQuery/`, `ReplComplete/`,
`ReplTui/`.

### Decision de design : pas de Tree-sitter

`REPL_IDEA.md` proposait Tree-sitter (C99). Ecarte : le projet interdit toute
dependance nouvelle (`nix/deps.nix`, pas de `subprojects/`). La coloration Volt
reutilise `Frontend::Lexer::Tokenize` sur un `StringInterner` et un `DiagEngine::Bag`
jetables. LLVM IR et ASM ont chacun un tokenizer C++ d'une centaine de lignes --
registres `%`/`@`, opcodes, types, litteraux.

---

## 3. Ce qui est fait -- moteur sans tete

Cette section documente le design tel qu'implemente, pour reference. Rien ici n'est
a ecrire.

### 3.1. `EvalUnit` -- le verbe JIT

`JitBackend::EvalUnit(Build, Unit)` (`JitBackend.cpp:533`). Decalque de `Reload`,
qui a deja resolu chaque sous-probleme :

1. `Opts = OneUnitOptions(Build, Unit)` : `SkipUnitsBelow = Unit.Ordinal`,
   `EntrySymbol = {}`, `bDefineSlotAccessor = false`,
   `bDefineCompilerSeamUnits = false`, `bReplaceUnit` selon que la ligne redefinit
   ou non.
2. `IrGenerator Line(Opts); Line.Begin(Build); Line.EmitUnit(Unit); Line.Finish();`
3. Dylib : `OpenGeneration` (dylib principale) si pas de redefinition,
   `OpenReplacement` (dylib laterale) sinon -- ORC refuse un doublon *dans* un
   dylib et l'accepte *entre* deux.
4. `Compiler.AddModules(Gen, TakeModules(Line))`.
5. Patch des slots : boucle identique a `Reload` (`SlotNameOf -> Lookup -> LookupIn
   -> store aligne`).
6. `LookupIn(Gen, "_V_init_" + Ordinal)`, appeler, lire l'etat d'exception.
   Sur exception non rattrapee : `RunResult{bOk=false, Message}` -- sans tuer
   la session.

Amorcage : le REPL compile stdlib + prelude, `Finalize()`, puis `Run()` une fois.
`__volt_entry` appelle `_V_init_all` et retourne 0 -- l'etat de la stdlib est
initialise sans API nouvelle.

### 3.2. `Evaluator` -- une session REPL

`source/Volt/REPL/ReplEval/Private/Evaluator.cpp` (525 lignes).

Possede `Driver::Driver` et `Backend::Jit::JitBackend`. `Start(Options)` compile
stdlib + prelude + materialise + `Run()`. `Feed(Line)` fait :

1. `IdentifiersIn(Line)` -- lexer la ligne pour les noms mentionnes.
2. `Driver::AppendUnit(Label, Text)` -- une unite de plus dans le `Driver`.
3. `BindResult(Ast, Serial)` -- si expression seule, la lier a `__volt_repl_<N>`.
4. `NameForeignStorage(Ast, Mentioned)` -- greffer des `@[External("volt", symbol)]
   external name : Type` synthetiques pour les variables de session mentionnees.
5. `Driver::AnalyzeUnit(Index)` -- parse, couture, sema, type-check.
6. `Harvest(Ordinal)` -- lire la portee racine, enregistrer les variables dans la
   table de session.
7. `Jit.EvalUnit(Build, View)` -- emettre et executer.
8. Si expression liee et type `Stringable` : `Feed("__volt_repl_echo(bound)\n",
   bMayEcho=false)`.

Variable `_` : dernier resultat. Re-pointe (meme symbole, type mis a jour) plutot
que reaffecte.

### 3.3. Portee de session

Table `SessionVar { Name, SemaTypeId, Symbol }` dans `Evaluator::State`.

Le `SemaTypeId` est valable d'une ligne a l'autre parce que chaque unite interne
dans le `TypeUniverse` du *build*. Le symbole (`_V_global_<ordinal>_<nom>`) est
stable parce que la ligne declarante n'est jamais reemise.

Greffage par AST synthetique (`TypeNodeFor` construit l'annotation de type
directement depuis le `SemaTypeId`, sans aller-retour texte). Filtrage par
`IdentifiersIn` pour que le cout par ligne soit O(mentions), pas O(session).

`UnitView::ExternalGlobals` (`BackendInput.hpp`) : `BindingSite -> symbol`.
`SlotFor` (`StmtLocalDeclEmitter.cpp`) consulte cette table et declare un global
externe au lieu d'en definir un neuf.

### 3.4. Classification multi-ligne

`ReplCore::Classify(Accumulated)` (`LineState.cpp`, 201 lignes).

Opere sur le flux de tokens de `Frontend::Lexer::Tokenize`, jamais sur le texte brut.
Un decompte textuel se ferait pieger par `"this is not the end"`, par `# def faux`,
et par les interpolations de chaine.

Trois axes :
- Blocs : `AlwaysOpens(Kind)` (`def`, `class`, `do`, `for`, ...) et
  `OpensAtStatementHead(Kind)` (`if`, `unless`, `while`, `until` -- seulement en
  tete de statement, pas en position de modifieur) contre `End`.
- Brackets : `(`, `[`, `{` contre `)`, `]`, `}`.
- Trailing : `WantsARightHandSide(Last)` -- operateur binaire sans operande droit.
- Erreur lexer (`TokenKind::Error`) : litteral non termine -> `NeedsMore`.

Direction unique dans le sens sur : un input incomprehensible vaut `Complete`, et le
compilateur rapporte l'erreur de syntaxe plutot que le prompt qui ne reviendrait jamais.

### 3.5. `ReplCommand` -- la commande CLI

`source/Volt/Volt/Private/Volt/CLI/Commands/ReplCommand.cpp` (211 lignes).

`TCommandRegister<FReplCommand>`. Options : `-O LEVEL`, `-e EXPR` (repetable),
`-v`, `--no-stdlib-cache`, `--fresh-stdlib`, `--no-stdlib`.

stdin non-TTY -> mode pipe, sans couleur et sans raw mode : c'est ce chemin que la
CI teste. stdout est un `std::cout` a `flush` avant chaque demi-ligne echo (la valeur
vient du fd, le type de C++).

### 3.6. Affichage actuel (pre-phase 3)

L'output est decoupe entre Volt et C++ :

```
Evaluator::Feed
   |
   +-- expression seule -> BindResult("__volt_repl_<N>")
   +-- AnalyzeUnit
   +-- EvalUnit
   +-- IsPrintable(Type) ?
       |
       +-- oui : Feed("__volt_repl_echo(bound)")
       |           -> Repl.vl imprime "=> value.inspect" sur fd 1
       |           -> ReplCommand.cpp ajoute " : Type\n" via std::cout
       |
       +-- non (pas d'inspect) :
       |       ReplCommand.cpp imprime "=> #<Type> : Type\n"
       |
       +-- ResultType vide (def, assignment) :
                rien
```

Le nom du type en string vient de `DescribeType()` qui interroge `TypeStore` cote
C++. Volt n'a pas aujourd'hui d'intrinsic pour stringify un type dans un generique --
pas de `name_of(T)`, pas de `T.name` dans une fonction standalone.

### 3.7. Diagnostics

`DiagEngine::Mark()` / `RenderSince(Mark)` / `HasErrorsSince(Mark)` / `TruncateTo(Mark)`
sont implementes dans `Core/DiagEngine.{hpp,cpp}`. `ConsumeLineDiagnostics(Mark, Out)`
dans `Driver`.

Une ligne qui ne compile pas est rapportee et la session continue. L'unite reste dans
le `Driver`, garde son ordinal, et n'est jamais emise.

Limite assumee : une ligne qui parse mais echoue en sema a deja publie ses
declarations dans `Registry`/`Types`. Il n'y a pas de rollback ; l'unite est marquee
morte et jamais emise. `:reset` le nettoierait en reconstruisant le `Driver`.

---

## 4. Ce qui reste -- phase 3 : affichage des valeurs et builtins

### 4.1. Systeme de coloration declaratif

IRB colore tout -- l'input en cours de frappe, le resultat `=> value`, le type, les
messages d'erreur -- et chaque categorie de token recoit une couleur distincte : un
nombre n'a pas la meme couleur qu'une chaine, qui n'a pas la meme qu'un mot-cle.
Volt doit faire au moins aussi bien.

Le systeme est **declaratif** : une palette unique, definie dans `ReplDoc`, consommee
par `ReplSyntax` (input et output), `ReplQuery` (builtins), `ReplComplete` (popup) et
`ReplTui` (rendu). Personne ne code en dur un `\x1b[32m`.

#### 4.1.1. La palette -- `ReplDoc/Palette.hpp`

Une structure plate, un champ par role semantique. Chaque champ est un `Color`
(triplet RGB 24 bits ou index 256 couleurs, avec un `EAttr` bold/dim/italic/faint).
Valeurs par defaut choisies pour un terminal sombre ; adaptable plus tard par
`$VOLT_REPL_THEME` ou `:theme`.

```cpp
struct Palette
{
    // -- Tokens Volt (input et output) -----------------------------------
    Color Keyword;          // def, class, if, end, return, ...
    Color Identifier;       // noms non resolus, variables locales
    Color TypeName;         // Int32, String, Array, Bool, ...
    Color FunctionName;     // appels de fonction, def name
    Color Number;           // 42, 3.14, 0xFF
    Color StringLiteral;    // "hello", interpolations
    Color Symbol;           // :foo
    Color BoolNil;          // true, false, nil
    Color Operator;         // +, -, *, ==, ...
    Color Comment;          // # commentaire, #{ doc }#
    Color Punctuation;      // (), [], {}, ,, ;

    // -- Resultat REPL ---------------------------------------------------
    Color ResultArrow;      // le "=>" lui-meme
    Color ResultValue;      // la valeur rendue (hors sous-tokens)
    Color ResultType;       // le " : TypeName" apres la valeur
    Color InspectBrackets;  // les #< > autour d'un objet non-printable

    // -- Interface -------------------------------------------------------
    Color Prompt;           // "volt>" et "    |"
    Color GhostText;        // suggestion en filigrane (Faint)
    Color Error;            // messages de diagnostic
    Color PanelBorder;      // bordures du panneau lateral
    Color PanelTitle;       // titre du panneau

    // -- Tokens IR / ASM (builtins :ir, :asm) ----------------------------
    Color IrRegister;       // %0, %name
    Color IrGlobal;         // @name
    Color IrOpcode;         // add, call, br, ret, ...
    Color IrType;           // i32, ptr, void
    Color AsmMnemonic;      // mov, add, ret, ...
    Color AsmRegister;      // rax, eax, xmm0, ...
    Color AsmImmediate;     // $42, 0x1234
};
```

`DefaultDarkPalette()` et `DefaultLightPalette()` sont des fonctions constexpr qui
rendent des `Palette` preconfigures. La detection dark/light se fait par la sequence
ANSI `\e]11;?\e\\` (reponse du terminal) ou `$COLORFGBG` -- si ni l'un ni l'autre ne
repond, dark par defaut.

#### 4.1.2. Coloration de l'input (pendant la frappe)

`ReplSyntax::HighlightVolt(text, palette)` rend un `Document` (liste de `Span`
colores). Appele a chaque frappe par `ReplTui` pour recolorer la ligne en cours.

Le lexer est `Frontend::Lexer::Tokenize` sur un `StringInterner` et un
`DiagEngine::Bag` jetables. Chaque `TokenKind` est mappe a un role de la palette :

| TokenKind | Role palette |
|---|---|
| `KwDef`, `KwClass`, `KwIf`, `KwEnd`, ... | `Keyword` |
| `Identifier` | `Identifier` (ou `TypeName` / `FunctionName` si la sema de session le sait) |
| `IntLiteral`, `FloatLiteral` | `Number` |
| `StringLiteral` | `StringLiteral` |
| `KwTrue`, `KwFalse`, `KwNil` | `BoolNil` |
| `Plus`, `Minus`, `Star`, `EqEq`, ... | `Operator` |
| `LParen`, `RParen`, `Comma`, ... | `Punctuation` |
| `Comment` | `Comment` |

Le mapping est une table statique (`TokenKind -> EPaletteRole`), pas un switch --
un token ajoute dans `TokenKind.inl` sans entree dans la table compile mais ne
colore pas (gris par defaut), plutot que de casser le build.

En phase completion, le contexte semantique est connu : `ReplComplete` peut annoter
les identifiants comme `TypeName` ou `FunctionName` selon ce que le `TypeStore`
contient. Sans completion active, tout identifiant est `Identifier`.

#### 4.1.3. Coloration du resultat (`=> value : Type`)

Le resultat n'est pas un blob bold uniforme. Il est **re-tokenize et colore par la
meme palette** que l'input. Concretement :

- Le texte produit par `.inspect` (ecrit par Volt sur fd 1) est capture par `ReplTui`
  (ou lu depuis un pipe interne), puis passe a `ReplSyntax::HighlightVolt` pour
  coloration token par token.
- Le suffixe ` : Type` est colore par `palette.ResultType`.
- Le prefixe `=> ` est colore par `palette.ResultArrow`.

Resultat :

```
volt> 42
=> 42 : Int32
   ^^          Number (ex: bleu)
      ^^^^^^^  ResultType (ex: gris dim)
^^             ResultArrow (ex: gris)

volt> "hello"
=> "hello" : String
   ^^^^^^^            StringLiteral (ex: vert)
           ^^^^^^^^   ResultType (ex: gris dim)

volt> true
=> true : Bool
   ^^^^          BoolNil (ex: magenta)
        ^^^^^^   ResultType (ex: gris dim)

volt> STDIN
=> #<IO> : IO
   ^^^^^       InspectBrackets + TypeName (ex: cyan)
       ^^^^^   ResultType (ex: gris dim)
```

La meme coloration s'applique dans le panneau lateral pour `:src`, `:ir`, `:asm`.

#### 4.1.4. Mode pipe -- zero couleur

Quand `!bInteractive` (stdin non-TTY, ou `-e`), aucun code ANSI n'est emis nulle
part. Ni dans l'input (pas de prompt), ni dans l'output. C'est ce chemin que la CI
teste. La palette n'est meme pas consultee.

#### 4.1.5. Coloration de l'echo -- piste C enrichie

Le split Volt/C++ pour l'echo est conserve (pas de `name_of(T)` encore). Mais le C++
ne se contente plus de bold/dim -- il re-tokenize le texte de `.inspect` :

Trois pistes d'analyse pour le suffixe type (reference `PLAN_REPL.md`) :

| Piste | Principe | Complexite | Maintenabilite |
|---|---|---|---|
| A : `macro def __type_name` dans chaque mixin | Tout-en-Volt, ~12 fichiers stdlib | Elevee | Fragile (contrat implicite inspect <-> __type_name) |
| B : intrinsic `name_of(T)` | Nouveau keyword lexer+parser+sema, tout-en-Volt | Elevee (epic complet) | Excellente, utile au-dela du REPL |
| C : valeur en Volt, type en C++ | Split naturel, ~10 lignes | Faible | Bonne |

Decision : **Piste C maintenant**, **Piste B plus tard** comme feature langage.

### 4.2. Prelude REPL

`source/Volt/REPL/Prelude/Repl.vl`, compile a l'amorcage **apres** la stdlib (donc
hors du prefixe `StdlibUnitCount`, sans effet sur `FrontendCacheKey`).

Etat actuel :

```volt
def __volt_repl_echo<T>( value : T ) -> Void
  IO.stdout.print( "=> " )
  IO.stdout.print( value.inspect )
end
```

A modifier (piste C, coloration) :

```volt
def __volt_repl_echo<T>( value : T ) -> Void
  IO.stdout.print( "\e[1m=> " )
  IO.stdout.print( value.inspect )
  IO.stdout.print( "\e[0m" )
end
```

La question de savoir si le type du resultat satisfait un trait d'affichage est deja
resolue par `IsPrintable` dans `Evaluator.cpp:292` -- il cherche `inspect` dans le
`TypeStore`. Quand le type ne l'a pas, `ReplCommand` affiche le type seul.

### 4.3. Generations ephemeres

`:type`, `:bench` et toute ligne avortee en sema produisent des modules qui ne
doivent **jamais** atteindre la `JITDylib` principale. Trois regles :

1. Une unite qui n'a pas passe sema n'est jamais emise -- pas de module.
2. `:type` s'arrete apres `Line.Finish()` : la generation n'est jamais ouverte,
   `TakeModules` n'est jamais appele, le `LLVMContext` du generateur meurt avec lui.
   (Rappel M3 : l'ordre de destruction `Closed`/`Ctx` dans `IrGeneratorState.hpp` a
   ete corrige pour ce cas -- un generateur abandonne.)
3. `:bench` a besoin d'executer, donc d'une generation. Elle est ouverte par
   `OpenReplacement` et **droppee** par `DropGeneration` des la derniere iteration
   rendue. Sur : `:bench` est synchrone et sans slot repointe vers lui.

Un compteur de generations vivantes, affiche par `:reset`, rend la fuite observable.

### 4.4. Module `ReplQuery` -- builtins

Nouveau : `source/Volt/REPL/ReplQuery/`. Pur (pas d'I/O). Rend des structures de
donnees que `ReplTui` (ou `ReplCommand` dans un premier temps) affiche.

| Commande | Source de verite | Notes |
|---|---|---|
| `:type <expr>` | Compiler l'expression, lire `UnitTypes::ExprType`, rendre le nom. **Jeter** la generation sans l'executer. | `EvalUnit` jusqu'a l'etape 4, sans l'appel de l'etape 6. |
| `:layout <T>` | `TypeStore -> LayoutId -> Layouts->Of` : taille, alignement, champs. | Rendu en table Unicode. |
| `:ir <sym>` | Promouvoir le hook `VOLT_JIT_DUMP_IR` de `JitCompiler::DumpIfAsked` en accesseur `LastUnitIr()`. | Colore par `ReplSyntax` (tokenizer LLVM IR). |
| `:asm <sym>` | `JitCompiler::Disassemble(addr, len)` via LLVM MC. Expose en `std::string` par `IJitBackend`, LLVM reste derriere le pimpl. | Colore par `ReplSyntax` (tokenizer ASM). |
| `:src <sym>` | `Member::Unit`/`Decl -> SourceRange -> SourceManager::TextOf`. | Colore par `ReplSyntax` (tokenizer Volt). |
| `:doc <sym>` | Meme `SourceRange`, remonter le texte en arriere jusqu'au bloc `#{ ... }#` qui precede. | Le lexer jette les commentaires de doc (`SkipCommentOrDoc`, aucun `TokenKind` Doc dans `TokenKind.inl`). `:doc` relit le texte du `SourceManager` autour du `SourceRange` de la declaration. |
| `:bench <expr>` | Unite synthetique `def __volt_repl_bench -> Void`, appelee N fois sous `steady_clock`. | Generation ephemere (voir 4.3). |
| `:history` | Etat de `ReplCore`. | |
| `:reset` | Reconstruire `Driver`, `JitBackend` et la table de session. Les anciennes generations ORC restent mappees (quelques dizaines de Ko). | Affiche le compteur de generations vivantes. |
| `:help` | Table des builtins. | |
| `:exit` | Quitter la session. | |

### 4.5. Module `ReplDoc` -- vocabulaire partage

Nouveau : `source/Volt/REPL/ReplDoc/`. Depend de `Core` seulement.

Vocabulaire : `Span` style (texte + couleur + attribut), `Document` (liste de spans),
`Table` (grille de cellules), `Pane` (cadre avec titre).

C'est le socle que `ReplQuery`, `ReplSyntax` et `ReplComplete` partagent avec `ReplTui`.
Le fait qu'il ne connaisse ni terminal ni compilateur est ce qui rend les cinq autres
testables sans TTY.

### 4.6. Module `ReplSyntax` -- coloration

Nouveau : `source/Volt/REPL/ReplSyntax/`. Depend de `ReplDoc` et `Frontend`.

Trois tokenizers :

1. **Volt** : `Frontend::Lexer::Tokenize` sur un `StringInterner` et un
   `DiagEngine::Bag` jetables. Les diagnostics d'une ligne incomplette sont ignores.
2. **LLVM IR** : tokenizer C++ d'une centaine de lignes. Registres `%`, globals `@`,
   opcodes, types (`i32`, `ptr`), litteraux.
3. **ASM** : tokenizer C++ d'une centaine de lignes. Registres, mnemoniques,
   immediats, labels.

Chaque tokenizer rend un `Document` (liste de `Span` styles). Pas de dependance LLVM.

---

## 5. Ce qui reste -- phase 4 : TUI

`ReplTui`, seul module avec de l'I/O. `termios` en mode brut, `TIOCGWINSZ` pour la
taille, ANSI 24 bits, **pas** d'ecran alterne (le scrollback reste utilisable, comme
irb).

### 5.1. Edition de ligne

Maison :
- Historique (fleches haut/bas).
- `^A` / `^E` / `^K` / `^W`.
- Recherche inverse (`^R`).
- Continuation multi-ligne : appelle `ReplCore::Classify` -- la TUI n'en
  reimplemente aucune partie, elle affiche seulement le prompt de continuation.

### 5.2. Panneau lateral (split-pane)

Affichage en deux colonnes quand le terminal a >= 100 colonnes :

```
+--- volt repl (JIT) -----------------------+--- doc: Array[T]::map -------------------+
| volt> numbers = [1, 2, 3, 4]              |                                          |
|  => [1, 2, 3, 4] : Array[Int32]           | Returns a new array with the results of  |
|                                           | running block once for every element.    |
| volt> :doc Array.map                      |                                          |
| ---> (affiche dans le volet droit)        | Signature:                               |
|                                           |   def map[U](&block: T -> U): Array[U]   |
| volt> :src Array.map                      |                                          |
| ---> (remplace par le code source)        | Examples:                                |
|                                           |   [1, 2, 3].map { |x| x * 2 }           |
| volt> numbers.map { |x| x * 10 }         |   # => [2, 4, 6]                         |
|  => [10, 20, 30, 40] : Array[Int32]       |                                          |
|                                           |                                          |
| volt> _                                   |                                          |
+-------------------------------------------+------------------------------------------+
```

- `:doc <Nom>` : documentation formatee (titres, signatures, exemples avec coloration
  syntaxique). Panneau lateral.
- `:src <Nom>` : code source exact avec numeros de ligne et coloration. Panneau lateral.
- `:ir`, `:asm` : panneau lateral ou inline selon la longueur.
- Comportement adaptatif : si < 100 colonnes, bascule en pager plein ecran
  temporaire (`q` pour fermer).

La mise en page vit dans `ReplDoc`, pas dans `ReplTui` -- c'est ce qui la rend
testable par goldens sans terminal.

### 5.3. Coloration live de l'input

Reutilise le systeme declaratif de la section 4.1. A chaque frappe (ou chaque
redraw) :

1. `ReplTui` passe le texte accumule a `ReplSyntax::HighlightVolt(text, palette)`.
2. `ReplSyntax` tokenize via `Frontend::Lexer`, mappe chaque `TokenKind` au role
   de la `Palette` via la table statique `TokenKind -> EPaletteRole`, et rend un
   `Document` (liste de `Span` colores).
3. `ReplTui` ecrit les `Span` avec les codes ANSI correspondants.

Une ligne incomplete (multi-ligne en cours) est coloree telle quelle -- les
diagnostics du lexer sont jetables et ignores, le code se colore meme avec des
erreurs de syntaxe. C'est le comportement d'IRB : `def f` non ferme colore `def`
en mot-cle et `f` en identifiant, sans attendre le `end`.

En phase completion, `ReplComplete` enrichit le `Document` : un identifiant que le
`TypeStore` reconnait comme type est re-annote `TypeName`, un appel connu est
re-annote `FunctionName`. Sans completion active, pas d'enrichissement.

### 5.4. Remplacement de `ReplCommand`

A la phase 4, `ReplCommand.cpp` delegue a `ReplTui` quand stdin est un TTY. Le mode
pipe reste inchange (pas de raw mode, pas de couleur dans le resultat) -- c'est ce
chemin que la CI teste.

---

## 6. Ce qui reste -- phase 5 : completion semantique

`ReplComplete`, pur, rend une liste de candidats ; `ReplTui` dessine la liste
deroulante.

### 6.1. Completion par point (`expr.`)

Typer le prefixe (chemin `:type`, generation jetee), puis parcourir `OwnMember` +
`Includes` (mixins) + `Super` pour le nominal, en rendant signature et type de retour.

```
volt> text = "hello world"

volt> text.
       +-------------------------------+
       | split(separator: String)      | --> [String]
       | slice(range: Range)           | --> String
       | size()                        | --> Int32
       | starts_with?(prefix: String)  | --> Bool
       +-------------------------------+
       (Fleches haut/bas pour naviguer, Tab pour valider)
```

### 6.2. Identifiant nu

Table de session d'abord, puis `FreeFunctions()`, puis les noms de types par
iteration `TypeCount()`/`Type(Id)`.

### 6.3. Commandes `:`

`:` en debut de ligne -> table des builtins. `:d` + Tab -> `:doc`.

### 6.4. Ghost-text

Plus long prefixe commun dans l'historique, rendu en `Faint` par `ReplDoc`.
Comme Fish shell.

---

## 7. Vision UX

Inspiration : Ruby irb.

### 7.1. Rendu attendu, mode interactif

```
volt repl -- ^D to leave
volt> 42
=> 42 : Int32
volt> puts "Hello"
Hello
volt> "test"
=> "test" : String
volt> x = [1, 2, 3]
=> [1, 2, 3] : Array[Int32]
volt> x.map { |n| n * 10 }
=> [10, 20, 30] : Array[Int32]
volt> :type x
Array[Int32]
volt> :layout Int32
+--------+------+-------+-------+
| Field  | Type | Size  | Align |
+--------+------+-------+-------+
| (self) | i32  | 4     | 4     |
+--------+------+-------+-------+
volt> def twice( n : Int32 ) -> Int32
    |   n * 2
    | end
volt> twice( 21 )
=> 42 : Int32
```

### 7.2. Rendu attendu, mode pipe (CI)

```
$ echo 'puts( 40 + 2 )' | volt repl
42
```

Pas de prompt, pas de couleur, pas de type annotation quand la sortie n'est pas un TTY.

### 7.3. Principes

- Le REPL est un langage Volt standard. Pas de syntaxe speciale, pas de mode
  "expression implicite" -- une expression seule est liee automatiquement et affichee,
  mais le mecanisme est une reecriture AST, pas une grammaire differente.
- `_` est la derniere valeur.
- Une ligne qui ne compile pas est rapportee et la session continue.
- Une exception non rattrapee est rapportee et la session continue.
- `:reset` reconstruit tout l'etat.
- Le REPL n'ajoute aucune dependance au projet.

---

## 8. Fichiers touches

### 8.1. Nouveaux (phases 3 a 5)

```
source/Volt/REPL/ReplDoc/
  meson.build
  Public/Volt/ReplDoc/Palette.hpp
  Public/Volt/ReplDoc/Span.hpp
  Public/Volt/ReplDoc/Document.hpp
  Public/Volt/ReplDoc/Table.hpp
  Public/Volt/ReplDoc/Pane.hpp
  Private/...

source/Volt/REPL/ReplSyntax/
  meson.build
  Public/Volt/ReplSyntax/Highlighter.hpp
  Private/VoltHighlighter.cpp
  Private/IrHighlighter.cpp
  Private/AsmHighlighter.cpp

source/Volt/REPL/ReplQuery/
  meson.build
  Public/Volt/ReplQuery/QueryEngine.hpp
  Private/QueryEngine.cpp

source/Volt/REPL/ReplComplete/
  meson.build
  Public/Volt/ReplComplete/Completer.hpp
  Private/Completer.cpp

source/Volt/REPL/ReplTui/
  meson.build
  Public/Volt/ReplTui/Terminal.hpp
  Private/Terminal.cpp
  Private/LineEditor.cpp
  Private/SplitPane.cpp

tests/repl/Query/...
tests/repl/Syntax/...
tests/repl/Complete/...
```

### 8.2. Modifies (phases 3 a 5)

- `source/Volt/REPL/Prelude/Repl.vl` -- coloration ANSI.
- `source/Volt/REPL/meson.build` -- ajouter les 5 modules.
- `source/Volt/Volt/Private/Volt/CLI/Commands/ReplCommand.cpp` -- delegation a
  `ReplTui` en mode interactif, coloration du suffixe ` : Type`.
- `source/Volt/meson.build` -- `subdir('REPL')` (deja present).
- `tests/meson.build` -- nouveaux tests.
- `BackendJIT/Public/Volt/BackendJIT/JitBackend.hpp` -- `Disassemble(addr, len)`
  pour `:asm`.
- `BackendJIT/Private/JitCompiler.{hpp,cpp}` -- `LastUnitIr()`, `Disassemble`.
- `BackendCore/Public/Volt/BackendCore/ExecutableBackend.hpp` -- `Disassemble` dans
  `IJitBackend`.

### 8.3. Existants, deja complets (ne pas toucher sauf bug)

- `source/Volt/REPL/ReplEval/` -- `Evaluator`.
- `source/Volt/REPL/ReplCore/` -- `Classify`.
- `source/Volt/Volt/Public/Volt/CLI/Commands/ReplCommand.hpp`.
- `source/Volt/Driver/Private/DriverRepl.cpp`.
- `source/Volt/Driver/Public/Volt/Driver/Driver.hpp` -- `AppendUnit`, `AnalyzeUnit`,
  `ViewOf`, `ConsumeLineDiagnostics`.
- `source/Volt/Backend/BackendJIT/Private/JitBackend.cpp` -- `EvalUnit`.
- `source/Volt/Core/.../DiagEngine.{hpp,cpp}` -- `Mark`/`RenderSince`/`TruncateTo`.
- `tests/repl/` -- 11 tests existants (`Eval/*`, `Classify/*`, `Recover/*`).

---

## 9. Verification -- portes sequentielles

Une porte n'est pas un jalon de reporting : rien de la phase suivante ne commence tant
qu'elle n'est pas verte. Chaque porte se clot par un handoff ecrit dans
`.agents/PROGRESS-issue-120.md`.

A toutes les portes, sans exception :

- `meson setup --reconfigure build && meson compile -C build` (les sources sont globees).
- **Non-regression** : les 549 tests actuels restent verts, sans toucher une fixture.
- **Invariants d'epic** : `grep -r BackendLLVM source/Volt/Backend/BackendJIT/` -> 0 ;
  aucun header LLVM dans un `Public/` ni dans `BackendCore`.

### Porte 1 (moteur sans tete) -- FRANCHIE

Voir `PROGRESS-issue-120.md`, phases 6 et 7.

### Porte 2 -- builtins et affichage (fin de la phase 3)

- `__volt_repl_echo` colore : bold pour la valeur, dim gray pour le type.
- Repli propre (`=> #<TypeName> : TypeName`) sur un type sans `inspect`.
- `Query/Type`, `Query/Layout`, `Query/Ir`, `Query/Asm`, `Query/Doc`, `Query/Src`,
  `Query/Bench` en mode batch (pipe).
- Le compteur de generations vivantes ne bouge pas apres 100 `:type` d'affilee, et
  revient a sa valeur d'avant apres 100 `:bench` -- preuve executable de la regle
  des generations ephemeres.

### Porte 3 -- TUI et interaction (fin des phases 4 et 5)

- **Purete d'I/O** : test CI qui grep `<iostream>|std::cout|std::cerr|printf|::write`
  sur `source/Volt/REPL/` en excluant `ReplTui/`, et echoue sur toute occurrence.
  (Note : `ReplCommand.cpp` est dans `source/Volt/Volt/`, pas dans `REPL/`, donc il
  n'est pas concerne par ce test.)
- Goldens purs sans terminal : `ReplSyntax` (spans d'une ligne connue, plus une ligne
  syntaxiquement invalide qui doit tout de meme se colorer), `ReplDoc` (grille rendue
  en large et en etroit, pour couvrir les deux modes de mise en page), `ReplComplete`
  (candidats apres `expr.`, apres un identifiant nu, apres `:`).
- Manuel : session interactive reelle -- raw mode, panneau lateral >= 100 colonnes,
  bascule pager en dessous, completion et ghost-text.

### Fin d'epic seulement

`tidy` une fois, puis `python3 scripts/graphify/update_graphify.py`. Rien n'est
commite par l'agent : le travail fini reste dans l'arbre de travail.

---

## 10. Reste a faire hors REPL

Points issus de `PROGRESS-issue-120.md`, section "Reste a faire". Non lies au REPL
lui-meme mais au BackendJIT :

1. **Patch a chaud d'un processus vivant.** M3 recharge puis re-execute. Le mecanisme
   (slot, dylib de generation, refus) est deja celui qu'il faut ; il manque un mode ou
   le programme tourne dans son propre thread pendant que la boucle surveille.

2. **Table de vtables et rechargement.** Une entree de vtable est un pointeur de
   fonction constant, pas un slot : une methode rechargee atteinte par `dyn Trait` garde
   l'ancien corps. Non detecte et non refuse aujourd'hui -- a traiter (patch des mots de
   vtable, ou refus quand un symbole recharge occupe une entree).

3. **`tidy`** : une seule fois, a la fin de l'epic.

4. **`python3 scripts/graphify/update_graphify.py`** : a la fin de l'epic.

### Rappel des contraintes non negociables

- `grep -r BackendLLVM source/Volt/Backend/BackendJIT/` : que des commentaires.
- Zero header LLVM dans `BackendJIT/Public/`.
- `ninja -C build` -- **jamais deux builds en parallele**.
- Rien n'est commite par l'agent : le travail fini reste dans le working tree.

---

## Analyse des pistes pour `name_of(T)` (reference, hors perimetre immediat)

Pour memoire, l'intrinsic `name_of(T)` permettrait de supprimer le split C++/Volt
pour le suffixe ` : Type` de l'echo. C'est la Piste B de `PLAN_REPL.md` :

- **Lexer** : `VOLT_KEYWORD(NameOf)` dans `TokenKind.inl`.
- **Parser** : parselet `name_of(Type)`, yield un `ExprNode`.
- **Sema** : resolution dans `TypeChecker` -- resout le type, stringify via
  `TypeStore::Describe`, remplace par un `StringLiteral`.
- **Backend** : rien, c'est un `StringLiteral` avant d'arriver au backend.

Utilite au-dela du REPL : debug, logging, serialisation, messages d'erreur. Mais
c'est un epic complet, a planifier separement.
