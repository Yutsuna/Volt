# Plan : Macros compile-time (résolution + lowering)

## Contexte

Volt a déjà la syntaxe macro **spécifiée dans les samples** (`samples/Syntax/Macros/Delegate.vl`, `Serializable.vl`, style Crystal) mais **zéro machinerie compilateur** : pas de keyword `macro`, pas de nœud AST, pas d'évaluateur. Les deux samples sont des fixtures réservées, explicitement exclues des goldens (`cmake/VoltTests.cmake:13`, `tests/GoldenTest.cmake:61`).

**Décisions actées (utilisateur)** :
- Expansion **textuelle Crystal-style** : corps de `macro def` capturé brut ; la passe évalue les tags `{% %}`/`{{ }}`, émet du texte Volt, ré-lexe/ré-parse avec le Lexer/Parser existants dans le même `AstContext`.
- Résolution **per-file** (pas d'export InterfaceRegistry en v1) → marche dans `parse --lowered` ET pipeline Full.
- Périmètre v1 : **les 2 samples parsent + s'expansent**, golden-testés.

Tout passe par les manifests existants (meta-first) : `TokenKind.inl`, `Nodes.inl`, `PassList.inl` + un nouveau manifest `MacroOps.inl` pour les opérations macro-time (zéro hardcode).

## Étape 0 — Vérifications rapides (graphify, pas de grep large)

Avant de coder, confirmer via `graphify explain` + lectures ciblées :
- Interface Lexer↔Parser (`Parser.hpp` / `Lexer.hpp`) : tokens pré-lexés en vecteur ou pull à la demande → conditionne la capture brute du corps.
- `Core::SourceManager` : ajout d'un buffer synthétique (pour lexer le texte généré avec des `SourceRange` valides).
- Comportement actuel d'un appel nu `delegate(...)` dans un DeclBlock de classe (`ParseDecl.cpp` → `ParseFieldOrMember`) : probablement rejeté aujourd'hui.
- Le lexer gère-t-il les doc-blocks `#{ ... #}` (L1-4 des deux samples) ? Sinon, à ajouter.

## 1. Lexer — `source/Volt/Frontend/Public/Volt/Frontend/Lexer/TokenKind.inl` + `Private/Lexer/`

- 1 ligne : `VOLT_KEYWORD( KwMacro, "macro" )` (bloc L88-119). Enum + spelling + keyword-lookup dérivés automatiquement.
- Doc-blocks `#{ ... #}` : skip comme commentaire multi-lignes si absent (petit ajout dans le chemin commentaire du lexer).
- **Pas** de tokens `{%`/`{{`  : le corps macro est du texte brut, les tags sont scannés par la passe.

## 2. AST — `Nodes.inl` + `Decl.hpp`

2 lignes manifest + 2 structs aggregates (Loc en premier, reflection P2996 automatique, pas de VOLT_FIELDS) :

```cpp
VOLT_DECL( MacroDef )      // Nodes.inl, bloc decls L61-70
VOLT_DECL( MacroInvoke )

// Decl.hpp
struct MacroDef    { Core::SourceRange Loc; Symbol Name; ParamList Params; Symbol BodyText; };  // corps brut interné
struct MacroInvoke { Core::SourceRange Loc; Symbol Name; ExprList Args; SymbolList ArgNames; }; // miroir des champs nommés de Call
```

Les types annotés des params (`Array<Symbol>`) sont parsés comme des `TypeNode` ordinaires et **ignorés** en v1 (zéro hardcode : aucun nom de type Volt en C++).

## 3. Parser — `Frontend/Private/Parser/ParseDecl.cpp`

Seuls édits manuels hors manifest (les deux switch ne sont pas générés) :
- `AtDeclaration()` (L45-64) : `case TokenKind::KwMacro: return true;` + reconnaître `Identifier (` en position decl (invocation macro).
- `ParseDeclaration()` (L66-96) : `case TokenKind::KwMacro: return ParseMacro();` + branche invocation → `ParseMacroInvoke()` (réutilise `ParseCallArguments` pour `delegate( methods: [...], to_field: :user )`).
- `ParseMacro()` (calqué sur `ParseClass` L163) : parse l'en-tête `macro def Name ( params )` avec la machinerie params existante, puis **capture brute du corps** jusqu'au `end` correspondant : scan du flux de tokens en comptant l'imbrication — ouvrants {`KwDef, KwDo, KwIf, KwUnless, KwWhile, KwFor, KwCase, KwClass, KwStruct, KwModule, KwMacro`} vs `KwEnd` ; corps = sous-chaîne source entre offsets, internée dans `BodyText`.
  - Invariant qui rend ce comptage correct : chaque directive `{% for %}`/`{% if %}` se ferme par `{% end %}` → les tokens `for`/`if`/`end` des tags restent équilibrés.

## 4. Passe Sema `MacroExpansion` — le cœur

- 1 ligne : `VOLT_PASS( MacroExpansion, 15, Lowering )` dans `Sema/Public/Volt/Sema/PassList.inl` (avant JsxLowering 20 / CaseLowering 22 → les macros peuvent émettre du JSX/case). Enregistrement/ordre/Driver : gratuits. Visible dans `volt parse --lowered` sans toucher au Driver.
- 1 champ `PassStats` : `MacrosExpanded`.
- Nouveau `Sema/Private/Passes/MacroExpansion.cpp`, calqué sur `CaseLowering.cpp` (classe Rewriter, copie du nœud par valeur avant `Context.Add` — hazard de realloc d'arena — puis write-back dans le slot).

Étapes de la passe :
1. **Résolution** : walk de `TopDecls` + DeclLists imbriquées (Module/Class/Struct/Mixin) → map `Name → DeclId` des `MacroDef` (per-file). Diag `unknown macro '<x>'` via `Context.Diags.Error(Loc, …)`.
2. **Binding des args** : évaluer les exprs d'invocation en `MacroValue`, matcher positionnels + nommés contre `Params` (diag arity/nom inconnu).
3. **Scanner de template** : scan **caractère par caractère** de `BodyText` (pas de tokens → trouve `{{ }}` même dans les littéraux string comme `"#{@{{ field }}}"`) → arbre de directives : fragments texte, `Splice{expr}`, `For{var[, idx], seq, body}`, `If{cond, then, else}`, fermés par `{% end %}`. Contenu des tags parsé **paresseusement à l'expansion** avec le `ParseExpr` Pratt existant (pas de nouvelle traversée).
4. **Évaluateur** (greenfield, ~150 lignes) :
   `MacroValue = std::variant<std::monostate, bool, std::int64_t, std::string /*symbol|text*/, std::vector<MacroValue>>` — représentation interne C++, aucun nom de type Volt. Sous-ensemble d'Expr évalué : littéraux (`:sym`, entiers, strings), noms (env : params + variables de boucle + index), arrays, binaires (comparaisons, arithmétique), membres via **nouveau manifest `Sema/Public/Volt/Sema/MacroOps.inl`** :
   ```cpp
   VOLT_MACRO_OP( Size, "size", /* lambda sur MacroValue séquence */ )
   ```
   → table-driven, une op = une ligne, pas de chaîne if/else.
5. **Émission** : concat fragments + splices stringifiés (symbol → texte, int → chiffres).
6. **Ré-lex/ré-parse** : buffer synthétique `SourceManager` (« expansion of macro 'x' »), Lexer + Parser existants avec une petite entrée publique type `ParseDeclsInto(AstContext&, …)` si absente (vérifié à l'étape 0) → `DeclId`s dans le **même** AstContext/Interner.
7. **Splice** : dans la DeclList du parent (copie-mutation-write-back du nœud Class), remplacer le slot `MacroInvoke` par les DeclIds générés.
8. **Fixpoint** : re-scanner les decls générés pour de nouveaux `MacroInvoke`, profondeur max 32 → diag `macro expansion too deep`.
9. Les `MacroDef` restent dans l'arbre (l'arena ne supprime pas ; le golden lowered montre defs intacts + invocations remplacées).

Ajout probable côté AST : accesseur `DeclCount()` sur `AstContext` (seul `ExprCount()` existe, L125-128).

## 5. Diagnostics

Erreurs de parse dans le texte généré : rapportées ancrées sur le `Loc` de l'invocation, message préfixé `in expansion of macro '<name>'`. Pattern : `Context.Diags.Error(Range, msg)` comme le parser (`Parser.cpp:48-51`).

## 6. Tests

- Dé-exclure `samples/Syntax/Macros/` dans **`cmake/VoltTests.cmake:13`** et **`tests/GoldenTest.cmake:61`**.
- `golden-update` → `Delegate.vl.golden` / `.lowered.golden` + idem Serializable ; vérifier à la main que le lowered montre les `def name`, `def email`, `to_json` générés.
- Corpus `source/Lib/**/*.vl` doit toujours parser (le keyword `macro` ne doit rien casser).

## Séquence d'implémentation (checkpoints)

1. Étape 0 (vérifs graphify) → Lexer (keyword + `#{ #}`) — checkpoint : `volt parse` sur les samples n'échoue plus qu'au parser.
2. Nœuds AST + `ParseMacro`/`ParseMacroInvoke` — checkpoint : `volt parse -i samples/Syntax/Macros/Delegate.vl` produit l'arbre brut (MacroDef + MacroInvoke).
3. Squelette de passe + résolution + diags.
4. Scanner + évaluateur + `MacroOps.inl`.
5. Ré-parse + splice — checkpoint : `volt parse --lowered` montre les méthodes générées.
6. Dé-exclusion tests + `golden-update` + `volt-build format` → build `-Werror` propre → `volt-build tidy` → `volt-build test` vert → `graphify update .`.

## Fichiers modifiés

| Fichier | Changement | ~lignes |
|---|---|---|
| `Frontend/.../Lexer/TokenKind.inl` | keyword | 1 |
| `Frontend/Private/Lexer/*.cpp` | doc-block `#{ #}` si absent | ~15 |
| `Frontend/.../AST/Nodes.inl` | 2 decls | 2 |
| `Frontend/.../AST/Decl.hpp` | 2 structs | ~15 |
| `Frontend/.../AST/AstContext.hpp` | `DeclCount()` | ~5 |
| `Frontend/Private/Parser/ParseDecl.cpp` | 2 cases + ParseMacro + ParseMacroInvoke + capture brute | ~90 |
| `Frontend/.../Parser/Parser.hpp` (+ .cpp) | entrée ré-parse (si besoin, étape 0) | ~15 |
| `Sema/.../PassList.inl` | 1 passe | 1 |
| `Sema/.../Pass.hpp` | 1 champ PassStats | 1 |
| `Sema/Public/Volt/Sema/MacroOps.inl` | **nouveau** manifest ops | ~10 |
| `Sema/Private/Passes/MacroExpansion.cpp` | **nouveau** : résolution, scanner, évaluateur, splice | ~400 |
| `cmake/VoltTests.cmake`, `tests/GoldenTest.cmake` | dé-exclusion | 2 |
| `tests/golden/samples/Syntax/Macros/*` | goldens générés | — |

## Vérification finale

```sh
volt parse -i samples/Syntax/Macros/Delegate.vl            # arbre brut : MacroDef + MacroInvoke
volt parse --lowered -i samples/Syntax/Macros/Delegate.vl  # def name / def email générés
volt parse --lowered -i samples/Syntax/Macros/Serializable.vl
volt-build format && volt-build && volt-build tidy && volt-build test
graphify update .
```

Garde-fou zéro-hardcode : le grep de `rules/zero-hardcode.md` reste vide sur Frontend/Sema (MacroValue est un variant C++ interne, aucun nom de type Volt).
