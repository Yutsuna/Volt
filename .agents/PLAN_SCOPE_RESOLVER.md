# PLAN_SCOPE_RESOLVER — Architecture de la passe `ScopeResolver` (Order 10)

**Portée du document :** conception détaillée, avant implémentation, de la passe
`ScopeResolver` annoncée en [PLAN.md §III/§IV](PLAN.md). Objectif : une
architecture **béton** (types exacts, fichiers exacts, signatures exactes) et
**modulaire** au sens meta-first — un nouveau nœud qui ouvre une portée doit
coûter une ligne, jamais un nouveau traversal écrit à la main.

---

## I. Principes directeurs

1. **Aucun `switch` sur les node kinds.** Le moteur de traversal est un unique
   visiteur récursif `Overloaded{ ... , [](auto&){ ForEachField(...) } }` :
   les nœuds qui *ouvrent* une portée ont un lambda dédié ; tout le reste
   descend automatiquement par réflexion (`Meta::ForEachField`), exactement
   comme `MarkMetadata` dans
   [TypeChecker.cpp:81-113](../source/Volt/Sema/Private/Passes/TypeChecker.cpp).
   Ajouter un nœud AST avec un champ `StmtList`/`ExprId` n'exige **aucune**
   modification de `ScopeResolver` — il est déjà traversé.
2. **Value AST, jamais de pointeurs.** Comme `UnitTypes`/`TypeStore`, la table
   de portées est une `Core::Arena<Scope, ScopeId>` : des `Id`s typés, pas de
   `Scope*`. Voir [rules/ast-value.md](rules/ast-value.md).
3. **Publié, pas recalculé.** `ScopeResolver` tourne une fois (Order 10, avant
   `TypeChecker` à Order 30) et publie une table plate indexée par `ExprId`/
   `StmtId` sur le `PassContext`. `TypeChecker` ne refait **aucune** résolution
   de nom : il interroge la table. C'est le sens exact du commentaire déjà en
   place dans `TypeChecker.cpp:160-163` (*"until ScopeResolver publishes a
   table"*).
4. **Un seul pass = une fonction pure sur `PassContext`.** Zéro état global,
   zéro singleton — cohérent avec le pipeline parallèle par fichier décrit
   dans [Pass.hpp:36-41](../source/Volt/Sema/Public/Volt/Sema/Pass.hpp).
5. **Zero-hardcode intact.** `ScopeResolver` ne connaît aucun type Volt : il
   ne fait que lier un `Symbol` (identifiant interné) à un site de déclaration
   AST. Aucune notion de type n'entre dans ce fichier.

---

## II. Modèle de données

Nouveau sous-module, miroir de `Layout/SemaType.hpp` :

```
source/Volt/Sema/Public/Volt/Sema/Scope/ScopeTable.hpp
source/Volt/Sema/Private/Passes/ScopeResolver.cpp        // sa propre TU, comme JsxLowering
```

### `ScopeId`

```cpp
struct ScopeTag {};
using ScopeId = Core::TypedId<ScopeTag>;
```

### `EScopeKind`

Sert uniquement au diagnostic et à la future analyse de captures (§VII) — pas
de logique de résolution ne branche dessus, la portée est structurelle, pas
sémantique :

```cpp
enum class EScopeKind : std::uint8_t
{
    Unit,    // portée racine d'un fichier (Module top-level)
    Type,    // corps d'une Class/Struct/Mixin/Component/Circuit — portée des membres
    Method,  // paramètres + corps d'un Method
    Block,   // paramètres + corps d'un Block (`do |x| ... end`, futur lambda)
    Branch,  // Then/Else d'un If, Body d'un While — un bloc `{}` lexical nu
};
```

### `Binding`

Un site de déclaration nommé. `Site` est délibérément un `std::variant`
d'`Id`s AST — jamais un pointeur, jamais une copie du nœud :

```cpp
using BindingSite = std::variant<Frontend::StmtId,   // LocalDecl
                                  Frontend::ParamId,  // paramètre de Method/Block
                                  Frontend::DeclId>;  // Field / InstanceVar

struct Binding
{
    Symbol       Name;
    BindingSite  Site;
    ScopeId      Owner;   // portée où la liaison est visible
};
```

### `Scope`

```cpp
struct Scope
{
    ScopeId                              Parent;   // invalide = racine
    EScopeKind                           Kind;
    Core::SmallVec<Symbol, 4>            Order;    // ordre déclaratif (diagnostics stables)
    std::unordered_map<Symbol, Binding>  Bindings; // shadowing : une entrée par scope, pas globale
};
```

### `ScopeTable` — le canal de publication

Même forme que `UnitTypes` (arène + index dense par `Id` AST) :

```cpp
class ScopeTable
{
public:
    [[nodiscard]] ScopeId PushScope ( ScopeId Parent, EScopeKind Kind );
    void Declare ( ScopeId Scope, Symbol Name, BindingSite Site );

    // Résolution complète, remontant la chaîne de parents — utilisée une
    // seule fois, par ScopeResolver lui-même, au moment de l'usage.
    [[nodiscard]] const Binding *Resolve ( ScopeId From, Symbol Name ) const;

    // --- Table publiée, consultée par les passes suivantes (O(1), pas de
    // remontée de chaîne) ---
    void BindUse ( Frontend::ExprId Use, const Binding &Target );
    [[nodiscard]] const Binding *BindingOf ( Frontend::ExprId Use ) const;
    void SetScopeOf ( Frontend::StmtId Stmt, ScopeId Scope );
    [[nodiscard]] ScopeId ScopeOf ( Frontend::StmtId Stmt ) const;

private:
    Core::Arena<Scope, ScopeId>                 Scopes;
    std::vector<const Binding *>                UseIndex;   // par ExprId::Value
    std::vector<ScopeId>                        StmtScope;  // par StmtId::Value
};
```

`Resolve` est **interne** à `ScopeResolver` (remontée O(profondeur)) ;
`BindingOf`/`ScopeOf` sont la **surface publique** consommée par
`TypeChecker` et par le futur backend — O(1), déjà résolus.

---

## III. Extension de `PassContext`

Un champ de plus, symétrique à `Values` :

```cpp
struct PassContext
{
    Frontend::AstContext &Ast;
    const TypeStore       &Types;
    UnitTypes             &Values;
    ScopeTable            &Scopes;   // NOUVEAU — sortie mutable de ScopeResolver
    Core::DiagEngine::Bag &Diags;
    PassStats             &Stats;
    const InterfaceRegistry *Globals = nullptr;
};
```

`ScopeTable` vit sur le `CompileUnit`, comme `UnitTypes` — un par fichier,
jamais partagé entre threads. `PassStats` gagne un compteur
`std::size_t ScopesResolved = 0;` / `std::size_t UnresolvedIdentifiers = 0;`
suivant la convention déjà en place ([Pass.hpp:24-32](../source/Volt/Sema/Public/Volt/Sema/Pass.hpp)).

---

## IV. Le moteur générique de traversal

Un seul visiteur récursif, dans `ScopeResolver.cpp`, sur le modèle exact de
`MarkMetadata` (`TypeChecker.cpp:81-113`) mais paramétré sur les trois arènes
(`StmtId`, `ExprId`, `DeclId`) :

```cpp
class Resolver
{
public:
    explicit Resolver ( PassContext &InContext ) : Context( InContext ) {}

    void Run ();  // itère les Method / top-level Decls de l'unité

private:
    // --- Nœuds qui OUVRENT une portée : un lambda chacun ---
    void WalkMethod ( const Frontend::Method &Node, ScopeId Parent );
    void WalkBlock  ( const Frontend::Block  &Node, ScopeId Parent );   // Expr Block
    void WalkIf     ( const Frontend::If     &Node, ScopeId Current );
    void WalkWhile  ( const Frontend::While  &Node, ScopeId Current );
    void WalkType   ( const Frontend::DeclNode &Node, ScopeId Parent ); // Class/Struct/Mixin/Component/Circuit

    // --- Nœuds qui DÉCLARENT dans la portée courante, sans l'ouvrir ---
    void WalkLocalDecl ( const Frontend::LocalDecl &Node, ScopeId Current );

    // --- Nœuds qui USENT un nom ---
    void WalkIdentifier  ( Frontend::ExprId Id, ScopeId Current );
    void WalkInstanceVar ( Frontend::ExprId Id, ScopeId Current );

    // --- Repli générique : descend par réflexion dans tout ExprId / ExprList
    // / StmtList / DeclList / ParamList que le nœud courant porte, sans
    // connaître son nom. C'est ce qui rend l'ajout d'un nœud "gratuit". ---
    void WalkStmt ( Frontend::StmtId Id, ScopeId Current );
    void WalkExpr ( Frontend::ExprId Id, ScopeId Current );

    PassContext &Context;
};
```

`WalkStmt`/`WalkExpr` font exactement ce que fait `MarkMetadata` :
`std::visit(Overloaded{ Cas spéciaux..., [&](const auto& Concrete){ if constexpr
(Reflected<decltype(Concrete)>) ForEachField(Concrete, [&](auto Name, auto&
Field){ /* dispatch par type de champ : ExprId → WalkExpr récursif, StmtList →
boucle WalkStmt, etc. */ }); } }, Node)`.

Les cas spéciaux (`Method`, `Block`, `If`, `While`, `LocalDecl`, `Identifier`,
`InstanceVar`) interceptent *avant* le repli générique ; tout le reste
(`Binary`, `Call`, `ArrayLit`, `Ternary`, `CaseExpr`, …) n'a **jamais besoin
d'être mentionné** — le générique les traverse pour trouver les usages
imbriqués.

### Pourquoi Order 10, avant `MacroExpansion`/`JsxLowering`/`CaseLowering`

`ScopeResolver` tourne **après** `EnumLowering` (12)... non — regarder
`PassList.inl` : l'ordre actuel est `PipelineLowering(8) → ScopeResolver(10) →
EnumLowering(12) → MacroExpansion(15) → JsxLowering(20) → CaseLowering(22) →
TypeChecker(30)`. `ScopeResolver` tourne donc **avant** les lowerings
syntaxiques suivants. Deux options à trancher explicitement à l'implémentation :

- **Option retenue (recommandée) :** `ScopeResolver` résout uniquement les
  identifiants **présents avant lowering** (le code source tel qu'écrit).
  `MacroExpansion`/`CaseLowering` introduisent de nouveaux nœuds *après* —
  ceux-là restent non résolus jusqu'à un second passage léger. C'est
  acceptable car ces passes de lowering sont elles-mêmes syntaxiques (comme
  `EnumLowering`, elles ne diagnostiquent pas) et n'introduisent pas de
  nouveaux noms libres — seulement des motifs déjà résolus (voir
  `CaseLowering.cpp`).
- Si l'implémentation découvre que `MacroExpansion` matérialise de vrais
  nouveaux identifiants non résolus, la table `ScopeTable` doit rester
  **incrémentale** (`Declare`/`BindUse` sont appelables après coup) — ne pas
  la figer en `const` avant `TypeChecker`. `MacroExpansion` pourra alors
  appeler `ScopeTable::Resolve` directement sur les identifiants qu'elle
  génère avant de les insérer, sans repasse complète.

---

## V. Portées ouvertes par nœud (table exhaustive)

| Nœud AST                              | `EScopeKind` | Déclare dans le parent | Ouvre pour |
|----------------------------------------|--------------|-------------------------|------------|
| `Module` (racine de l'unité)           | `Unit`       | —                        | tout le fichier |
| `Class` / `Struct` / `Mixin` / `Component` / `Circuit` | `Type` | `Field`, `Method` (par nom) | corps de la classe (accès `self.x` sans qualification dans les méthodes se résout via `WalkInstanceVar`, pas via ce scope) |
| `Method` (Decl)                        | `Method`     | ses `Params`             | son `Body` |
| `Block` (Expr — `do |x| ... end`)      | `Block`      | ses `Params`             | son `Body`, et capture lexicale du parent (§VII) |
| `If` (Stmt)                             | `Branch` × 2 | —                        | `Then` et `Else` séparément (une variable de `Then` n'est jamais visible dans `Else`) |
| `While` (Stmt)                          | `Branch`     | —                        | `Body` |
| `LocalDecl` (Stmt)                      | *(déclare, n'ouvre pas)* | son `Name` dans le scope courant | reste de la liste de `Stmt` courante — géré naturellement car un `StmtList` est parcouru dans l'ordre avec le même `ScopeId` |

`Field`/`InstanceVar` ne passent **pas** par la remontée de chaîne de
`Scope` : ce sont des membres résolus par nom-sur-receveur, déjà le rôle de
`LookupOn`/`MemberByDecl` (`TypeStore.hpp`). `ScopeResolver` note juste
qu'une `InstanceVar` est un usage valide *si* le scope courant est en
contexte d'instance (`bStaticContext` — logique déjà existante côté
`TypeChecker`, réutilisée telle quelle, voir §VII).

---

## VI. Résolution des usages & diagnostics

`WalkIdentifier` :

```cpp
void Resolver::WalkIdentifier ( Frontend::ExprId Id, ScopeId Current )
{
    const auto &Node = std::get<Frontend::Identifier>( Context.Ast.Expr( Id ) );
    if ( const Binding *Found = Context.Scopes.Resolve( Current, Node.Name ) )
    {
        Context.Scopes.BindUse( Id, *Found );
        return;
    }
    // Pas d'erreur ici : peut être un nom de type, une méthode statique, un
    // membre — TypeChecker tranche avec le contexte de type. ScopeResolver
    // ne connaît que les *bindings de valeur locales* (params + locals).
}
```

Point important : **`ScopeResolver` ne remplace pas la résolution de
membres/types**, il ne couvre que ce que `Checker::Locals` couvrait déjà
(paramètres + `LocalDecl`). Un identifiant non trouvé dans `ScopeTable` n'est
**pas** une erreur à ce stade — `TypeChecker` retente la résolution comme nom
de type / appel global. Ça garde la passe strictement dans son rôle
(*Analysis* qui *publie*, jamais qui *diagnostique* l'absence — cf. le même
principe déjà appliqué par `EnumLowering`, qui ne diagnostique jamais et
laisse `TypeChecker` le faire).

Diagnostic que `ScopeResolver` **émet** lui-même, car c'est strictement une
propriété de la structure de portées (aucune info de type requise) :

- **Redéclaration dans le même scope** (`local x` deux fois dans le même
  `Method`/`Block`/`Branch`) → erreur, un nom ne peut être déclaré deux fois
  au même niveau lexical. Le masquage (*shadowing*) d'un `Scope` parent par
  un `Scope` enfant reste légal et silencieux (cas d'usage courant :
  `x = x + 1` dans un `if` qui masque un paramètre du même nom).

---

## VII. Migration de `TypeChecker.cpp`

Remplace `Checker::Locals` (`std::unordered_map<Symbol, SemaTypeId>` plat,
`TypeChecker.cpp:163`) par une deuxième table, cette fois **indexée par
`BindingSite`** (pas par `Symbol`) — c'est ce qui élimine le bug latent du
plat actuel : deux `local x` dans des scopes frères (jamais actifs en même
temps mais partageant la même clé `Symbol` aujourd'hui) ne peuvent plus se
confondre.

```cpp
// Avant (TypeChecker.cpp:163) :
std::unordered_map<Symbol, SemaTypeId> Locals{};

// Après :
std::unordered_map<Frontend::ParamId /*ou StmtId*/, SemaTypeId> LocalTypes{};
```

Au site d'usage (`TypeChecker.cpp:589`, `Locals.find(Expr.Name)`) :

```cpp
// Avant : Locals.find( Expr.Name )
// Après :
if ( const Binding *Bound = Context.Scopes.BindingOf( ExprIdOfIdentifier ) )
{
    const SemaTypeId Type = LocalTypes[Bound->Site];   // clé structurelle, plus de collision
}
```

`EnterMethod` (`TypeChecker.cpp:301-327`) perd son `swap` manuel de
`Locals`/`UnconstrainedVarInitializers` — le scope stack existe déjà,
`ScopeResolver` a fait le travail une fois pour toutes. `TypeChecker` garde
`UnconstrainedLiterals`/`UnconstrainedVarInitializers` tels quels (Point 0 du
PLAN.md, orthogonal aux portées : c'est de l'inférence de type, pas de la
résolution de nom).

---

## VIII. Extensibilité — pourquoi cette forme précisément

- **Nouveau nœud qui ouvre une portée demain** (ex. un futur `Lambda` /
  `MatchArm`) : une ligne dans la table §V + un `case` de plus dans
  `Overloaded{...}` du visiteur. Le reste du compilateur (backend, closures)
  n'a rien à changer, il lit `ScopeTable`.
- **Captures de closures (`Block`/futur `Lambda`)** : `Scope::Parent` forme
  déjà la chaîne lexicale nécessaire. Une passe de conversion de closures
  ultérieure n'a qu'à walker `BindingOf` de chaque usage dans un `Block` et
  comparer `Owner` à l'`ScopeId` du `Block` lui-même — tout usage dont
  `Owner` est un ancêtre est une capture. Zéro nouveau traversal AST, juste
  une requête sur la table déjà publiée.
- **Backend (allocation de frame/stack)** : chaque `Binding.Site` a une
  identité stable (`StmtId`/`ParamId`) et un `Owner` (`ScopeId`) — assez pour
  dériver la durée de vie et le rang d'imbrication sans repasser par l'AST.
- **Aucun coût si non utilisé** : passes qui ne touchent pas aux noms
  (`EnumLowering`, `PipelineLowering`) ignorent simplement le nouveau champ
  `Scopes` de `PassContext`, comme elles ignorent déjà `Types`/`Values`.

---

## IX. Plan de validation

1. `volt-build format test` — 77/77 verts, y compris les tests d'inférence de
   littéraux (Point 0) qui exercent `EnterMethod`/`Locals` aujourd'hui : ils
   doivent produire des résultats identiques après migration vers
   `ScopeTable`.
2. `volt check source/Lib/` — 13/13 fichiers stdlib toujours sans erreur
   (aucune régression de résolution de `self`/paramètres/locals).
3. Nouveau test ciblé : shadowing dans un `if` imbriqué (`Checker::Locals`
   actuel ne peut pas déjà couvrir ce cas — c'est la dette du Point 5) et
   redéclaration dans le même scope (doit désormais diagnostiquer).
4. `graphify update .` pour publier `Scope/ScopeTable.hpp` et
   `Passes/ScopeResolver.cpp` dans le graphe.
