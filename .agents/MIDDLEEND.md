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

## Passes du MiddleEnd

### 1. Order 10 — `ScopeResolver` (`source/Volt/Sema/Private/Passes/ScopeResolver.cpp`)
- Parcourt l'AST de manière réflexive pour construire la table des portées lexicales (`ScopeTable`).
- Déclare les paramètres et variables locales dans leurs portées respectives (`Method`, `Block`, `Branch`).
- Valide la structure des portées (détecte les redéclarations illégales dans la même portée, autorise le masquage/shadowing dans des portées enfants).

### 2. Order 30 — `TypeChecker` (`source/Volt/Sema/Private/Passes/TypeChecker.cpp`)
- Effectue l'inférence de type bidirectionnelle.
- Gère la table des types (`TypeStore`), le liage des types natifs (`TypeBinder`), et la table des types d'unités (`UnitTypes`).
- Gère la table des littéraux non contraints (`UnconstrainedLiterals`) et des initialiseurs (`UnconstrainedVarInitializers`).
- Valide les arités d'appels, les membres d'instance vs statiques (`bStaticContext`), et les signatures de fonctions.

---

## Produit en sortie du MiddleEnd
En sortie du MiddleEnd, l'AST/HIR est :
- **100 % Résolu** : Chaque variable et identifiant est lié de manière univoque à son site de déclaration dans la `ScopeTable`.
- **100 % Typé** : Toutes les expressions et littéraux ont un type exact et vérifié (`SemaTypeId`).
- **100 % Validé** : Zéro ambiguïté ou erreur sémantique résiduelle. Les diagnostics d'erreurs sémantiques sont tous émis.
