# PLAN — chantier Sema (branche `Feat/Add-Semantic`)

État de référence : build vert, 77/77 tests, `volt check source/Lib/` propre.
Ce fichier recense les 6 points ouverts identifiés lors de la revue du
chantier TypeStore/TypeBinder/TypeChecker (SemaType, UnitTypes, TypeResolve).

---

## 0. Principe de design — inférence paresseuse des littéraux numériques

**Statut : gap de design identifié (2026-07-21), pas encore implémenté.**

Comportement voulu par l'utilisateur, à documenter avant tout autre travail
sur `CheckCallArgs`/`LiteralType` :

Un littéral numérique nu (`10`, `8`, `@capacity * 2`, …) n'a **pas** de type
concret au moment où il est écrit — seulement "un type numéro". Son type
réel doit rester indéterminé jusqu'à sa **première utilisation qui le
contraint** : un appel dont le paramètre a un type déclaré, une assignation
vers une variable déjà typée, un `return` vers un type de retour déclaré,
etc. Cette première contrainte *confirme* le type a posteriori — elle ne le
convertit pas, elle le *révèle*.

Exemple donné par l'utilisateur :

```
def func( n : UInt32 ) -> Int32
  n
end

func 10   # valide : 10 s'infère en UInt32 au moment de l'appel
```

Mais une fois qu'une valeur a déjà été *explicitement* typée (paramètre
annoté, variable déclarée avec un type), elle n'est plus sujette à cette
inférence — un second usage incompatible reste une vraie erreur :

```
def func( n : UInt32 ) -> Int32
  return n   # FAUX : n est explicitement UInt32, pas convertible en Int32
end
```

Cas concret qui a motivé cette note — `source/Lib/Primitives/Array.vl:29-30` :

```
new_cap = @capacity == 0 ? 8 : @capacity * 2
new_buf = Pointer<T>.malloc( new_cap )
```

`new_cap` n'a jamais été typé explicitement — c'est un `Int32`/`UInt64`/...
indéterminé ("un type numéro") jusqu'à ce que son passage à `malloc`
(paramètre `UInt64`) le confirme. Le diagnostic actuel
(« argument 1 to malloc has type Int32, expected UInt64 », voir point 1)
est donc un **faux positif** : ce n'est pas un bug de `Array.vl`, c'est que
`LiteralType()` (`TypeChecker.cpp:635`) fixe le type d'un `IntLiteral` de
façon rigide et immédiate via `LookupNodeKind("IntLiteral")` — un seul type
de la stdlib peut réclamer ce node kind (`@[Literal(IntLiteral)]`), donc tout
littéral entier est câblé sur ce type unique dès sa création, sans jamais
repasser par une étape d'unification tardive. **Ne pas corriger `Array.vl`
pour faire taire ce diagnostic** — le corriger là serait masquer le vrai
manque : l'absence de typage différé/unification dans le checker.

Implémentation non commencée — pistes possibles (à valider avant de lancer
un agent dessus) :
- Un littéral numérique produit un `SemaTypeId` *provisoire/polymorphe*
  (pas encore lié à un `NominalId` concret) jusqu'à sa première contrainte.
- `CheckCallArgs` (point 1) devient alors le point de résolution : quand
  l'argument est un littéral non contraint, il adopte le type du paramètre
  au lieu de déclencher un diagnostic de mismatch.
- Une fois résolu, le type doit se propager à toute autre occurrence de la
  même expression logique (ex: `new_cap` réutilisé ligne 38) — donc la
  résolution doit écrire dans `Ctx.Values`/`UnitTypes`, pas seulement dans
  un slot local à l'appel qui a déclenché la confirmation.
- Distinguer "valeur encore librement inférable" de "valeur déjà typée
  explicitement" (annotation de paramètre, type déclaré sur une variable) —
  seule la première catégorie doit bénéficier de cette souplesse ; la
  seconde reste une erreur stricte de mismatch, comme aujourd'hui.

---

## 1. Vérification des arguments d'appel

**Statut : ✅ Terminé & validé (2026-07-22)**

`CheckCallArgs( Loc, Resolution, Args )` a été implémenté et validé par l'agent `volt-reviewer`.
- Supporte les `Call` classiques (via la table `CalleeResolution`) et les `DotCall`.
- Vérifie l'arité (nombre d'arguments requis vs fournis).
- Vérifie la correspondance des types paramètre / argument et émet les diagnostics d'erreur appropriés.
- Conforme à la règle zero-hardcode, value-AST et style Unreal C++.

## 2. `DotCall` non câblé sur la résolution de membre

**Statut : ✅ Terminé & validé (2026-07-22)**

`DotCall` est entièrement câblé dans `TypeChecker.cpp:518-527` :
- Routé via `LookupOn( SelfValue, Ctx.Ast.Text( Expr.Method ) )` + `CheckCallArgs`.
- Évalue correctement les arguments et renvoie `Found.Result`.
- Validé par l'agent `volt-reviewer` (zero-hardcode, value-AST, cpp-style).

## 3. `bSelf` jamais consulté (statique vs instance)

**Statut : ✅ Terminé & validé (2026-07-22)**

Implémenté dans `TypeChecker.cpp` :
1. `NakedTypeExprs` (ensemble de `ExprId`) suit les expressions représentatives d'une référence de type nue (`Pointer`, `Pointer<T>`, ou `self` dans un contexte statique).
2. `bStaticContext` conserve le contexte statique (`def self.foo`) vs instance lors du parcours des méthodes (`EnterMethod`).
3. `CheckMemberSelf` et `CheckDotCallSelf` valident que `bSelf` correspond au récepteur (accès statique sur type vs instance, et appel dans un contexte statique).
4. `volt-build format` propre et 77/77 tests passés avec succès.

## 4. `MemberByDecl` ne discrimine pas par unité

**Statut : ✅ Terminé & validé (2026-07-22)**

`MemberByDecl` dans `TypeStore.hpp` prend désormais `std::uint32_t Unit` en paramètre et compare à la fois `Entry.Unit == Unit` et `Entry.Decl == Decl`. Les appels dans `TypeBinder.cpp` ont été mis à jour pour transmettre `Unit`. Validé par build et tests (77/77).

## 5. `Checker::Locals` — table plate, dette assumée

**Statut : accepté comme dette, pas un bug à corriger dans ce chantier.**

`Checker::Locals` est une table plate par corps de méthode ("jusqu'à ce que
ScopeResolver publie une vraie table" — commentaire du fichier lui-même) :
pas de blocs imbriqués, pas de shadowing correct. C'est un raccourci
documenté, pas une régression — ne pas y toucher tant que `ScopeResolver`
n'existe pas.

## 6. Documentation / graphify — ménage de fin de chantier

**Statut : graphify fait ; format/build restent à faire.**

- `graphify-out/GRAPH_REPORT.md` régénéré le 2026-07-21 22:41 — le graphe
  reflète l'état du code à ce moment-là.
- `volt-build format` pas encore relancé sur les changements en cours dans
  `TypeChecker.cpp` / `TypeBinder.cpp` / `TypeStore.hpp` (points 1/2, encore
  non finalisés).
- `source/Volt/Core/Public/Volt/Core/Meta/Reflect.hpp` est modifié dans
  l'arbre de travail : garde `__has_include(<meta>)` autour de
  `#include <meta>`. Hors périmètre des points 1/2, contraire à la règle du
  projet d'ignorer clangd (`.agents/rules/cpp-style.md` — GCC via
  `volt-build` est la seule vérité ; GCC a `<meta>` sans condition). Décision
  à prendre : garder ou revert.
- Reste à faire une fois les points 1/2 finalisés : trancher sur
  `Reflect.hpp`, puis `volt-build format` → `volt-build` → `volt-build test`
  → un nouveau `graphify update source/Volt` si le code a encore changé
  depuis la dernière régénération.

---

## Ordre de traitement recommandé

1. ✅ **Terminé** : points 1 et 2 (vérifiés et validés par `volt-reviewer`).
2. ✅ **Terminé** : point 3 (`bSelf` - statique vs instance validé, 77/77 tests passés).
3. ✅ **Terminé** : point 4 (`MemberByDecl` par unité validé, build & 77/77 tests passés).
4. Point 5 : ne pas toucher (dette documentée, attend `ScopeResolver`).
5. Point 6 : à refaire systématiquement à la fin de chaque point ci-dessus.
