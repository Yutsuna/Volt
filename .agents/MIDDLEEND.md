# MiddleEnd Architecture — Semantic Analysis & Lazy-Strict Typing

## Rôle du MiddleEnd
Le MiddleEnd de Volt prend en entrée l'AST désucré issu du Frontend et effectue la **résolution des portées (Scope Resolution)**, le **liage des types (Type Binding)**, et la **propagation des contraintes sémantiques**.

Sa philosophie fondamentale est le **"Lazy typage dynamique, mais strict"**.

---

## Philosophie : "Lazy Typage Dynamique, mais Strict"

Le MiddleEnd ne précipite jamais le verrouillage d'un type. Si un littéral ou une variable n'a pas de type explicite, il est conservé dans un état **"potentiellement ce type"** (non contraint) et s'affine au fur et à mesure de l'utilisation. En revanche, dès qu'un type est **explicitement déclaré**, il devient **strict et immuable** (aucune conversion implicite autorisée).

### Exemple concret :

```volt
def func( a : UInt64 ) -> Int32
   a
end

b = 10
func b
```

### Déroulement pas-à-pas dans le MiddleEnd :

1. **Assignation implicite (`b = 10`)** :
   - Le Frontend produit un littéral entier `10` et la déclaration de `b`.
   - Le MiddleEnd enregistre `10` comme un `IntLiteral` **non contraint** avec `Int32` comme simple fallback par défaut.
   - `b` reçoit comme type l'expression non contrainte de `10`. Le type de `b` n'est **pas bloqué** à `Int32`.

2. **Appel de fonction (`func b`)** :
   - La signature de `func` exige un paramètre `a` de type **`UInt64`**.
   - Lors de la vérification de l'appel (`CheckCallArgs`), la contrainte `UInt64` est **répercutée en amont** (propagation récursive des contraintes `ConstrainExprType`) sur l'argument `b`, puis sur son initialiseur littéral `10`.
   - Le littéral `10` et la variable `b` sont ainsi affinés et verrouillés à **`UInt64`**.

3. **Valeur de retour explicite (`a` de type `UInt64` vers `Int32`)** :
   - Le paramètre `a` est **explicitement typé** en `UInt64`. Son type est connu avec certitude et **strict**.
   - Le corps de la fonction tente de retourner `a` alors que la signature indique une valeur de retour `Int32`.
   - **Erreur sémantique immédiate** : `a` étant explicitement un `UInt64`, Volt refuse toute conversion implicite vers `Int32`.

---

## Passes du MiddleEnd (`EPassKind::Analysis`)

Le liage des types (`Sema::BindUnitTypes`, `Layout/TypeBinder.cpp`) tourne
**avant** la phase parallèle, dans le seam sériel du Driver : un `10` d'un
fichier utilisateur résout vers l'`Int32` déclaré dans `source/Lib/`, donc ce
liage est cross-unit et ne peut pas être une passe par-fichier. Il enregistre
aussi les **noms de modules** (`TypeStore::DeclareModule`) : un `module` est un
namespace, pas un type nominal — ses méthodes sont des fonctions libres.

### 1. Order 10 — `ScopeResolver`
- Construit la `ScopeTable` (portées `Method`/`Block`/`Branch`), déclare
  paramètres et locales, calcule les captures de closures et leur `bEscapes`.
- Rejette les redéclarations dans une même portée, autorise le shadowing enfant.

### 2. Order 30 — `TypeChecker` (`Sema/Private/Passes/TypeChecker/`)
- Inférence bidirectionnelle sur `UnitTypes` ; `UnconstrainedLiterals` /
  `UnconstrainedVarInitializers` gèrent l'affinement lazy des littéraux nus.
- **Ordre impératif à chaque site d'affectation : inférer → contraindre →
  vérifier.** Contraindre avant d'inférer gèle les enfants (le parent est
  mémoïsé, le walk n'a jamais lieu).
- **Assignabilité totale** : un prédicat unique `TypeCompat::IsAssignable`
  (+ `CheckAssignable( ..., EAssignSite )`) est câblé sur les 5 sites
  (`LocalDecl`, `Assign`, `Return`, valeur finale de corps, défaut de
  paramètre). Un type **explicitement déclaré** est strict : aucune conversion
  implicite, **sauf** l'élargissement scalaire de même famille sans
  rétrécissement — décision de sémantique du langage, forcée par `hash -> UInt64`,
  détaillée dans [`rules/zero-hardcode.md`](rules/zero-hardcode.md).
- **Opérateurs** : `MemberType` enregistre la résolution dans `CalleeResolution`
  pour `Binary`/`Unary` comme pour `Member` — sur layout primitif/pointeur le
  backend émet l'instruction, sinon il appelle la méthode. Zéro passe, zéro
  nœud. Un opérateur exempté de corps doit **quand même** être déclaré.
- `Nil` (`@[Literal( NilLiteral )]`) assignable à tout `Pointer` ; `T?`
  (`NilableType`) **refusé bruyamment** (`nilable types are not implemented`).
- Arités, membres instance vs statique (`bStaticContext`), fonctions libres.

### 3. Order 40 — `AstInvariant`
Le garde-fou qui rend « 0 dette » **structurel** plutôt qu'instantané. Ne crée
aucun nœud (seule façon de tourner après `TypeChecker` sans casser l'invariant
structurel). Deux vérifications, toutes deux erreurs dures :
- **Aucun sucre résiduel** (ensemble `VOLT_EXPR_SUGAR` généré par le manifeste).
- **Typage total en position de valeur**, `Context.Metadata` et corps génériques
  exclus (cf. le contrat d'entrée backend dans
  [`rules/core-ast.md`](rules/core-ast.md)).

Compteurs remontés par `volt check --metrics` (`PassStats`, agrégé par réflexion).

> **Invariant structurel** : aucune passe créatrice de nœuds après `TypeChecker`.
> C'est ce qui garantit que tout nœud vu par un backend a un type.

---

## Produit en sortie du MiddleEnd
En sortie du MiddleEnd, l'AST/HIR livré au backend (voir
[`rules/core-ast.md`](rules/core-ast.md) pour le contrat complet — **27 nœuds
noyau**, `CalleeResolution` et `ClosureEnvFrame` déjà calculés) est :
- **100 % Résolu** : chaque identifiant lié à son site de déclaration.
- **100 % Typé** — avec une seule nuance, portée par le contrat : typé d'emblée
  dans du code concret, **typé après substitution** dans un corps générique
  (`T` de `Array<T>` ne devient un type qu'à la monomorphisation = codegen).
- **100 % Validé** : tous les diagnostics sémantiques sont émis.
