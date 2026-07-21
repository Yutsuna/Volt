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

**Statut : en cours (agent `volt-sema-pass` relancé sur ce point).**

`Member::Params` (types de paramètres résolus en phase B du `TypeBinder`,
`TypeStore.hpp:68`) n'était lu nulle part dans `TypeChecker.cpp` — `CallType()`
inférait chaque argument sans jamais le comparer à la signature résolue.
Concrètement, `10.times("mauvais type")` ne produisait aucun diagnostic.
Seul l'arité des génériques (`CheckArity`) était vérifiée, pas celle des appels.

À la relecture du fichier pendant que l'agent travaille (2026-07-21), une
fonction `CheckCallArgs( Loc, Resolution, Args )` est apparue et est appelée
depuis la branche `DotCall` (`TypeChecker.cpp:525`) — l'agent semble avoir
commencé l'implémentation. **Non re-vérifié** : couverture de `Call` classique
(pas seulement `DotCall`), messages de diagnostic, règles de coercion
littéral→primitif alignées sur le reste du checker. À confirmer au retour de
l'agent.

## 2. `DotCall` non câblé sur la résolution de membre

**Statut : en cours (même agent, même relance).**

`.méthode(args)` (nœud `DotCall`, champ `ExprList Args`) tombait dans la
branche catch-all de `Compute()`, traité comme un littéral inconnu — aucun
diagnostic (car `HasChildNodes` renvoie vrai) mais un type résultant invalide
en silence. `Index` (`a[b]`) résolvait déjà correctement via `LookupOn`/
`LookupMember`; `DotCall` devait recevoir le même traitement.

Vu à la lecture (2026-07-21) : `TypeChecker.cpp:518-527` route maintenant
`DotCall` via `LookupOn( SelfValue, Ctx.Ast.Text( Expr.Method ) )` +
`CheckCallArgs`, exactement comme prévu par le commentaire du code
("CaseLowering — order 22 — réécrit déjà les DotCall trouvés dans un pattern
`when`; ceux qui arrivent ici nomment une méthode directement sur le type
englobant"). **À confirmer** : build/tests verts avec ce changement.

## 3. `bSelf` jamais consulté (statique vs instance)

**Statut : recherché, pas implémenté.**

`Member::bSelf` (`TypeStore.hpp:74`, `def self.malloc` vs méthode d'instance)
n'est vérifié ni par `LookupMember` (`TypeStore.hpp:214`) ni par `LookupOn`
(`TypeChecker.cpp:372`). Appeler une méthode statique sur une instance (ou
l'inverse) résout sans erreur. Le golden path (`Pointer.malloc`) fonctionne,
rien n'empêche le cas symétrique fautif.

**Cause racine, plus profonde qu'un simple `if` manquant** : `SemaType`
(`SemaType.hpp:38`, juste `{NominalId Base, Args}`) ne distingue pas "le type
`Pointer` utilisé comme valeur" de "une instance de `Pointer`". Preuve : dans
`Compute()`, la branche `Identifier` (`TypeChecker.cpp:461-463`) fait
`MakeType(*Named, {})` quand le nom résout via `LookupType`, produisant un
`SemaTypeId` de forme identique à une vraie instance du même nominal.
L'information "statique vs instance" est perdue au retour d'`InferExpr`.

**Plan de correction proposé** (non implémenté) :
1. Table parallèle à `Ctx.Values.OfExpr` (par `ExprId`) marquant si
   l'expression est une "référence de type nue" — mise à `true` uniquement
   dans la branche `Identifier` qui fait `MakeType(*Named, {})`.
2. Aux points d'appel de `LookupOn` ayant accès à l'`ExprId` du receiver
   (`Member` ligne 469, `Index` ligne 473, `DotCall`/`self` implicite ligne
   524), comparer ce flag à `Found.Decl->bSelf` : receiver statique +
   `bSelf == false` → diagnostic ; receiver instance + `bSelf == true` →
   diagnostic.
3. Cas `self` implicite (`DotCall`, `SelfExpr`) : `SelfValue` est fixé une
   seule fois par corps de méthode (`TypeChecker.cpp:233`) sans distinguer
   `def foo` de `def self.foo` — même le contexte "je suis dans une méthode
   statique" n'est pas tracké aujourd'hui. Ajouter un `bool bStaticContext`
   à côté de `SelfValue`, dérivé du `bSelf` du membre en cours de check.

Hors périmètre de la relance en cours (l'agent a été explicitement instruit
de ne pas y toucher).

## 4. `MemberByDecl` ne discrimine pas par unité

**Statut : identifié, pas creusé en détail, pas implémenté.**

`TypeStore.hpp:190` (`MemberByDecl`) compare seulement `Entry.Decl == Decl`,
jamais `Entry.Unit`. `DeclareType` documente explicitement que redéclarer un
nom de type dans deux fichiers est supporté ("last stdlib definition wins",
`TypeStore.hpp:136-138`). Si un même nom de type est un jour redéclaré dans
deux unités, la phase A empile les `Members` des deux unités sur le même
`NominalId` sans jamais vider l'ancienne liste, et deux `DeclId` de valeur
identique (normal — chaque unité renumérote depuis 0) peuvent alors se
confondre dans `MemberByDecl`.

Latent, inoffensif tant que la stdlib ne redéclare pas un nom sous le même
identifiant — mais bug réel si le cas se présente. Correction probable :
ajouter `Unit` à la comparaison dans `MemberByDecl`, en passant l'unité
appelante en paramètre (elle est disponible partout où `MemberByDecl` est
actuellement appelé, à vérifier).

Hors périmètre de la relance en cours.

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

1. ✅ En cours : points 1, 2 (agent `volt-sema-pass`, relancé).
2. Ensuite : point 3 (bSelf) — plan ci-dessus prêt à être donné à un agent.
3. Puis : point 4 (MemberByDecl par unité) — bug latent, faible urgence tant
   que la stdlib ne redéclare pas de nom.
4. Point 5 : ne pas toucher (dette documentée, attend `ScopeResolver`).
5. Point 6 : à refaire systématiquement à la fin de chaque point ci-dessus.
