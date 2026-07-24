# Frontend Architecture — Parsing & Syntactic Lowering

## Rôle du Frontend
Le Frontend de Volt a pour rôle de transformer le code source en un **Arbre de Syntaxe Abstraite (AST)** propre et d'**abaisser tout le sucre syntaxique**. 

Le Frontend est **lazy** : il ne tente jamais de deviner ou de verrouiller le type des expressions ou des identifiants lorsqu'il y a un doute. Son objectif est de produire un AST sans ambiguïté syntaxique, prêt à être analysé par le MiddleEnd.

---

## Composants du Frontend

### 1. Lexer (`source/Volt/Frontend/Lexer/`)
- Transforme le texte source en un flux de jetons (`Token`).
- Défini de manière déclarative par les manifestes de tokens (`TokenKind.inl`).

### 2. Parser (`source/Volt/Frontend/Parser/`)
- Combinaison d'un parseur d'expressions **Pratt** (basé sur la puissance de liaison / precedence) et d'un parseur à **descente récursive** pour les déclarations et instructions.
- Construit l'AST en mémoire arène (`Arena<Node, Id>`) sans pointeurs nus (Value AST).

### 3. Lowering Syntaxique (`EPassKind::Lowering`)
Ces passes réécrivent le sucre en formes AST canoniques **avant** toute analyse
de type. Elles sont exactement celles que `volt parse --lowered` exécute, dans
l'ordre du manifeste `Sema/PassList.inl` (la numérotation est l'`Order`, pas un
compteur contigu). Chacune **balaye l'arène par index** et ne tient jamais une
référence à travers un `Add()` — l'idiome obligatoire est dans
[`rules/ast-rewrite.md`](rules/ast-rewrite.md).

- **8 — `FunctionalLowering`** : sections d'opérateurs/méthodes (`&.+ 5`,
  `&.trim`), captures (`&Math.square`), compositions (`>>`) → nœuds `Lambda`
  canoniques à symboles uniques (`AstContext::MakeUniqueSymbol`).
- **9 — `PipelineLowering`** : `x |> f` → `f( x )`.
- **12 — `EnumLowering`** : énumérations → types + constantes.
- **15 — `MacroExpansion`** : macros déclaratives + annotations `@[...]`. Retire
  les `MacroDef` de `TopDecls` en fin de passe (le nœud reste dans l'arène).
- **16 — `MagicExpansion`** : `__FILE__` / `__LINE__` et consorts.
- **20 — `JsxLowering`** : `<Button />` → `Volt.JSX.create_element( ... )`.
- **22 — `CaseLowering`** : `case/when` → liste plate de `WhenClause`
  (`pattern === target`), jamais une chaîne de `If` (§4.3 de
  [`rules/core-ast.md`](rules/core-ast.md) : `CaseExpr` reste **noyau**).
- **23 — `DotCallLowering`** : un `.method` en position d'instruction →
  `self.method( ... )`. Juste après `CaseLowering`, pour ne pas voler les
  `DotCall` du motif `when .even?`.
- **24 — `AssignLowering`** : `x op= v` → `x = x op v`. L'opérateur de base est
  **dérivé de l'orthographe** (`+=` moins son `=`), pas d'une table.
- **25 — `IndexLowering`** : `o[ a ]` → `o.[]( a )`, `o[ a ] = v` → `o.[]=( a, v )`.
  Le cas composé `o[ a ] += v` tombe de la composition avec `AssignLowering` :
  aucun code dédié.
- **26 — `InterpLowering`** : `"a#{ x }b"` → concaténation à gauche via
  `x.to_string`. Après `MacroExpansion`, car une macro peut générer du `#{ ... }`.

`"to_string"`, `"[]"`, `"[]="` sont des **noms de méthodes**, pas des noms de
type Volt : le garde-fou `ZeroHardcode` reste vert. Voir
[`rules/zero-hardcode.md`](rules/zero-hardcode.md).

---

## Produit en sortie du Frontend
En sortie du Frontend (`volt parse --lowered`), l'AST est :
- **Entièrement désucré** : aucun des **9 nœuds sucre** (`Interp`, `Index`,
  `DotCall`, `Section`, `Composition`, `Pipeline`, `JsxElement/Fragment/Text`,
  marqués `VOLT_EXPR_SUGAR` dans `Nodes.inl`) ne survit. La passe `AstInvariant`
  (Order 40) le vérifie **mécaniquement** à chaque build ; `tests/AstInvariant.cmake`
  le rejoue sur tout le corpus. Voir [`rules/meta-first.md`](rules/meta-first.md).
- **Non typé strict** : les littéraux comme `10` restent nus, non contraints.
- **Prêt pour le MiddleEnd** : aucune hypothèse de type verrouillée trop tôt.
