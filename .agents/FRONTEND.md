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

### 3. Lowering Syntaxique (Passes purement syntaxiques)
Le Frontend contient les passes de lowering qui réécrivent le sucre syntaxique vers des formes AST canoniques avant l'analyse sémantique :
- **Order 8 — `FunctionalLowering`** : Désucre les sections d'opérateurs/méthodes (`&.+ 5`, `&.trim`), les captures (`&Math.square`) et les compositions (`>>`) en nœuds `Lambda` canoniques avec symboles uniques (`AstContext.MakeUniqueSymbol`).
- **Order 9 — `PipelineLowering`** : Désucre les chaînages d'opérateurs pipeline (`x |> f`) en appels explicites `f(x)`.
- **Order 12 — `EnumLowering`** : Désucre les énumérations en structures de types et constantes.
- **Order 15 — `MacroExpansion`** : Étend les macros déclaratives et les annotations (`@[...]`).
- **Order 20 — `JsxLowering`** : Réécrit la syntaxe JSX/UI (`<Button />`) en instanciations de composants.
- **Order 22 — `CaseLowering`** : Réécrit les blocs `case/when` en arbres de décisions conditionnelles (`If/Else`).

---

## Produit en sortie du Frontend
En sortie du Frontend (`volt parse --lowered`), l'AST est :
- **Entièrement désucré** (les formes complexes sont réduites à des primitives AST standard).
- **Non typé strict** (les littéraux comme `10` restent des littéraux nus non contraints).
- **Prêt pour le MiddleEnd** (aucune hypothèse de type n'a été verrouillée prématurément).
