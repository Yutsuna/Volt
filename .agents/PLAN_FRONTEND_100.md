# PLAN — Clôture du Frontend + MiddleEnd (« 100 %, 0 dette, mergeable »)

**Objectif unique :** à la fin de ce plan, `Frontend` + `Sema` sont *terminés*.
Le seul travail restant sur Volt est l'écriture des 3 backends, par
pattern-matching déclaratif sur un **AST noyau** dont le contenu est un contrat
écrit, vérifié mécaniquement à chaque build.

À la clôture, `.agents/PLAN.md` **et** ce fichier sont supprimés : ce sont des
journaux de chantier, pas de la documentation. Ce qui doit survivre part dans
`.agents/rules/` (§7).

---

## 0. Ce que l'audit a réellement trouvé (2026-07-24)

L'analyse de départ (Interp / DotCall / Index non abaissés) est **exacte mais
très incomplète**, et une de ses causes est mal identifiée. Mesures faites sur
`build/bin/Volt_d` à `ba75efd`.

### 0.1 Recensement objectif des nœuds résiduels

Recensement de tous les nœuds survivant à `parse --lowered` sur
`samples/**` + `source/Lib/**` :

```sh
for f in $(find samples source/Lib -name "*.vl" -o -name "*.vlx"); do
  ./build/bin/Volt_d parse --lowered --no-color --no-location "$f"
done | grep -oE '(^|[│├└─ ]+)[A-Z][A-Za-z]+' | tr -d '│├└─ ' | sort | uniq -c | sort -rn
```

Sucre survivant : `Interp` ×21, `ArrayLit` ×23, `Index` ×13, `GenericInst` ×14,
`HashLit` ×10, `CaseExpr` ×10, `SizeOf` ×2, `DotCall` ×2, `Section` ×1,
`MacroDef` ×2. **Zéro** `Jsx*`, `Pipeline`, `Composition` : ces trois-là sont
réellement finis.

### 0.2 Les cinq trous que l'analyse de départ n'avait pas vus

Chacun est reproduit ci-dessous. Ce ne sont pas des « le backend devra
réimplémenter » : ce sont des **silences du middle-end**, donc des bugs.

**T1 — Corruption mémoire : une passe de réécriture tient une référence
d'arène à travers `Add()`.** `Arena` stocke dans un `std::vector` ; `Add()`
invalide toute référence. `FunctionalLowering::WalkExpr` reçoit `ExprId &Id`
lié *dans le nœud parent vivant* (via `ForEachField` sur
`Context.Expr(Parent)`), puis appelle `LowerSection(Id)` qui fait 6 `Add()`.
L'écriture `Id = ...` part dans de la mémoire libérée. Résultat : l'abaissement
réussit ou échoue **selon la taille du fichier**.

```
pad=0 residual Section=1     pad=4 residual Section=0
pad=1 residual Section=1     pad=5 residual Section=0
pad=2 residual Section=0     ...
```
(le même programme, précédé de *N* `def` sans rapport). C'est de l'UB, non
déterministe, et c'est l'unique cause du `Section` résiduel de
`samples/Sema/CallableArgs.vl`. `JsxLowering`, `PipelineLowering`,
`CaseLowering`, `EnumLowering` utilisent au contraire un balayage par index
(`for Index in 0..OriginalCount` + affectation dans le slot) : **sûr par
construction**.

**T2 — Le chaînage à point en tête n'est pas parsé.** Dans
`samples/Functional/FunctionalSpec.vl:64-67` :

```volt
clean_users = raw_users
  .filter( (&.empty?.!) )
  .map( transform )
```

est parsé en **trois instructions** : `clean_users = raw_users`, puis deux
`DotCall` orphelins. C'est un bug du parseur (aucune continuation de
`ParsePostfix` par-dessus un `Newline`), pas une lacune d'abaissement.
Conséquence directe : **abaisser `DotCall` en `self.m(...)` sans corriger
le parseur d'abord fige une sémantique fausse dans l'AST.** L'ordre des
travaux du §3 en découle.

**T3 — Aucun contrôle d'assignabilité, nulle part.** Le seul site typé du
compilateur est `CheckCallArgs`. Tout le reste passe en silence :

```volt
def r -> Int32
  "plain"          # accepté
end
def r2 -> Int32
  return "plain"   # accepté
end
def probe -> Void
  a : Int32 = 1
  a = "plain"      # accepté
end
struct Box
  getter v : Int32
  def set -> Void
    @v = "plain"   # accepté
  end
end
```
→ `OK : 17 file(s) type-checked`. Ni `LocalDecl` annoté, ni `Assign`, ni
`Return`, ni l'expression finale d'un corps, ni l'affectation de champ ne
sont vérifiés.

**T4 — `Index` ne produit aucun type.** `MemberType(..., IndexOperator)`
retourne un `SemaTypeId` invalide sur `Array<Int32>` :
`take( arr[0] )` avec `take( n : Int32 )` ne diagnostique rien, alors que
`take( "plain" )` diagnostique correctement.

**T5 — `Interp` ne produit aucun type.** Aucun type ne réclame le nœud
`Interp` (`@[Literal]`), donc `LiteralType` retombe silencieusement sur
l'invalide. `"a#{1}".nonexistent_method` ne produit **aucune** erreur, là où
`arr.nonexistent_method` en produit une.

**T6 (annexe) — `NilLiteral` et `NilableType` ne sont réclamés par aucun
type.** `source/Lib/Primitives/Exception.vl:41` écrit `if symbols != null` et
ne type-check que grâce à T3. Il n'existe ni `struct Nil`, ni sémantique pour
`T?`.

### 0.3 Conclusion de l'audit

Le middle-end n'est pas « complet mais avec du sucre résiduel ». Il est
**complet sur les chemins couverts par un sample, et muet ailleurs**. Le
chantier n'est donc pas « 3 petits abaissements » mais :

1. rendre les réécritures sûres (T1),
2. réparer la grammaire (T2),
3. abaisser le sucre restant (Interp / Index / DotCall),
4. rendre le typage **total** (T3, T4, T5),
5. **rendre les deux invariants vérifiables mécaniquement**, sinon la dette
   revient au premier commit suivant.

Le point 5 est le cœur du plan : c'est la seule chose qui transforme
« 0 dette aujourd'hui » en « 0 dette structurellement ».

---

## 1. Le contrat : l'AST noyau

Livrable principal, à figer **avant** d'écrire une ligne : la liste exacte des
nœuds que les 3 backends doivent savoir traiter. Tout le reste doit avoir
disparu à `EPassKind::Analysis` terminé.

### 1.1 Nœuds noyau (les backends les matchent)

| Catégorie | Nœuds | Ce que le backend en fait |
|---|---|---|
| Terminaux | `IntLiteral` `FloatLiteral` `StringLiteral` `CharLiteral` `BoolLiteral` `NilLiteral` `SymbolLiteral` `ArrayLit` `HashLit` | matérialise une constante / un agrégat depuis le `MemoryLayout` du type réclamant (`@[Literal]`) |
| Accès | `Identifier` `InstanceVar` `SelfExpr` `SuperExpr` `Member` `Deref` | load / GEP |
| Opérations | `Call` `Assign` `Ternary` | appel via `CalleeResolution`, store, select/branch |
| Opérateurs | `Binary` `Unary` | **uniquement** sur un receveur de layout `Primitive`/`Pointer` → instruction machine choisie sur `Primitive{Spelling,Bits}`. Sur tout autre layout : abaissé en `Call` (§3.4) |
| Contrôle | `CaseExpr` `BeginExpr` `RaiseExpr` | chaîne de tests / table de saut ; EH |
| Fermetures | `Lambda` `Block` | fonction + `ClosureEnvFrame` (taille, alignement, `bEscapes` déjà calculés) |
| Inertes | `GenericInst` `SizeOf` | ne portent aucune valeur runtime : le backend lit `Values.Get(Id)` (resp. la taille du layout) et ne descend jamais dedans |

### 1.2 Nœuds sucre (doivent avoir disparu)

`Interp`, `Index`, `DotCall`, `Section`, `Composition`, `Pipeline`,
`JsxElement`, `JsxFragment`, `JsxText`.

### 1.3 Les trois arbitrages, et pourquoi

Ils changent la charge des backends ; ils sont tranchés ici, explicitement.

**`ArrayLit` / `HashLit` restent noyau.** Ce sont des *littéraux*, pas du
sucre : ils sont déjà entièrement typés (`[1,2,3]` → `Array<Int32>`,
`arr.push` résout). Les abaisser en `Array.new` + `push` demanderait un nœud
séquence et une passe **après** `TypeChecker` (l'argument générique n'est
connu qu'à ce moment), donc une passe qui doit annoter à la main les types
des nœuds qu'elle crée — exactement la dette qu'on cherche à éviter. Le
mécanisme `NominalType::LiteralSlots` (`TypeStore.hpp`) a été construit pour
ce cas précis : le backend lit les slots et émet l'agrégat. Même statut que
`StringLiteral`, qui est aussi un agrégat `{data,size}`.

**`CaseExpr` reste noyau.** Après `CaseLowering` (order 22), c'est déjà une
liste plate de `WhenClause{ Patterns: [expr Bool], Body }` + `ElseBody` :
strictement une structure de contrôle, au même titre que `If` et `While` que
personne ne demande d'abaisser. L'abaisser en chaîne de `If` imposerait une
passe post-`TypeChecker` (l'exhaustivité a besoin des types) *et* détruirait
la seule information permettant une jump table LLVM. Coût backend de le
garder : ~15 lignes, identiques à une chaîne de `If`.

**`Binary`/`Unary` restent noyau, mais seulement sur layout primitif.** C'est
déjà la règle du projet (`rules/zero-hardcode.md` §3 : « the spelling selects
the instruction »). La nouveauté est l'autre moitié : sur un layout
`Aggregate` (`String + String`), l'opérateur **doit** devenir un `Call`, sinon
chaque backend refait la résolution de membre. Abaissement §3.4.

---

## 2. Phase A — Sûreté des réécritures (bloquant, corrige T1)

Rien d'autre ne peut être validé tant qu'une passe de réécriture peut perdre
son écriture selon la taille du fichier.

**A.1 — Convertir `FunctionalLowering` au balayage par index.**
`source/Volt/Sema/Private/Passes/FunctionalLowering.cpp` : supprimer
intégralement `FunctionalRewriter::WalkDecl/WalkStmt/WalkExpr/WalkFields`
(~120 lignes) et adopter la boucle de `PipelineLowering.cpp:30-44` :

```cpp
const std::size_t OriginalCount = Context.ExprCount();
for ( std::size_t Index = 0; Index < OriginalCount; ++Index )
{
    const ExprId Id{ static_cast<ExprId::ValueType>( Index ) };
    switch ( KindOf( Context.Expr( Id ) ) )
    {
        case ExprKind::Section:     Context.Expr( Id ) = LowerSection( Id );     break;
        case ExprKind::Composition: Context.Expr( Id ) = LowerComposition( Id ); break;
        default: break;
    }
}
```

`LowerSection`/`LowerComposition` retournent désormais un `ExprNode` (valeur),
plus un `ExprId`. Ils copient déjà leur nœud source (`const Section Sec =
std::get<Section>(...)`), donc ils sont sûrs dès qu'on ne réécrit plus à
travers une référence de champ. L'affectation `Context.Expr(Id) = f(Id)`
séquence l'appel avant l'évaluation du membre gauche (C++17), donc l'`Add()`
interne ne peut pas invalider la cible.

> **Pourquoi c'est *moins* de code, pas plus.** Le walker réflexif de
> `FunctionalLowering` était un doublon de `Meta::ForEachField` que
> `rules/meta-first.md` interdit explicitement. Le balayage par index n'a
> besoin d'aucun parcours : tous les nœuds vivent à plat dans l'arène.

**A.2 — Auditer les autres passes.** `JsxLowering` (l.93), `PipelineLowering`,
`CaseLowering`, `EnumLowering` sont conformes au balayage par index et copient
leur nœud source : vérification par lecture, aucune modification attendue.
`MacroExpansion` / `MagicExpansion` ne font aucun `Add()`.

**A.3 — Empêcher la récidive.** Ajouter
`.agents/rules/ast-rewrite.md` : *une passe de réécriture balaye l'arène par
index, copie le nœud source par valeur, et affecte le slot ; elle ne tient
jamais une référence d'arène à travers un `Add()`* — avec le contre-exemple
mesuré ci-dessus. Référencé depuis `.claude/CLAUDE.md` et
`.agents/skills/add-sema-pass`.

**A.4 — Test.** `samples/Sema/SectionBlockArg.vl` : la reproduction de
`CallableArgs.vl` précédée de 0 puis de 6 `def` de remplissage, en deux
fichiers, avec golden `parse --lowered` — le golden échoue aujourd'hui sur
l'un des deux et passera sur les deux.

**Validation A :** `volt-build format test` vert ; le recensement §0.1
retourne `Section` ×0 ; run `volt-build debug asan` sur
`samples/Sema/CallableArgs.vl` sans rapport (aujourd'hui : heap-use-after-free
attendu).

---

## 3. Phase B — Grammaire et abaissements

### B.1 — Continuation par point en tête (corrige T2) — *avant* B.3

`source/Volt/Frontend/Private/Parser/ParseExpr.cpp`, boucle `ParsePostfix` :
avant de rendre la main, si le token courant est `Newline`, regarder au-delà
de la suite de `Newline` ; si le premier token significatif est `Dot`,
consommer les `Newline` et continuer la chaîne postfixe.

Restreint à `TokenKind::Dot` uniquement — jamais `LBracket` ni `LParen`, qui
sont ambigus avec un littéral tableau ou une parenthèse en début
d'instruction. `AmpDot` (`&.`) est une section, pas une continuation :
exclu.

Couverture : `samples/Syntax/Expressions/LeadingDotChain.vl` (chaîne sur
3 lignes, commentaire intercalé entre deux maillons, chaîne dans un argument),
+ golden `parse`. Correction attendue de
`samples/Functional/FunctionalSpec.vl` : les deux `DotCall` orphelins
disparaissent au profit d'une unique chaîne `Call(Member(Call(Member(...))))`.

> C'est ici que se joue le « 0 mistake » demandé : sans B.1, B.3 réécrirait
> `.filter(...)` en `self.filter(...)` et graverait un contresens dans l'AST
> livré aux backends.

### B.2 — `Index` → `Call` (order 24, corrige aussi T4)

Nouvelle passe `IndexLowering`, une ligne dans `PassList.inl`. Balayage par
index (§A.1). Trois formes, dans cet ordre :

| Source | Résultat |
|---|---|
| `obj[ a, b ]` (lecture) | `Call( Member( obj, "[]" ), [a, b] )` |
| `obj[ a ] = v` | `Call( Member( obj, "[]=" ), [a, v] )` |
| `obj[ a ] += v` | `Call( Member( obj, "[]=" ), [a, Binary( +, Call( Member( obj, "[]" ), [a] ), v )] )` |

Le cas 2 et le cas 3 se déclenchent sur un `Assign` dont le `Target` est un
`Index` : la passe scanne donc **`Assign` puis `Index`**, et un `Index` déjà
consommé comme cible d'affectation ne doit pas être réécrit une seconde fois
(marquer les `ExprId` traités dans un `unordered_set` local, ou traiter les
`Assign` en premier passage et les `Index` restants en second).

Attention au cas 3 : `obj` est **dupliqué** dans l'AST (lu puis écrit). C'est
correct tant que `obj` est un `Identifier`/`InstanceVar`/`SelfExpr` (pas
d'effet de bord). Si `obj` est autre chose (`f()[i] += 1`), la passe émet le
diagnostic `"compound index assignment requires a simple receiver"` plutôt que
de dupliquer un appel — refuser explicitement vaut mieux qu'évaluer deux fois.

`"[]"` / `"[]="` ne sont pas du hardcode de type : ce sont les *orthographes
d'opérateurs* du langage, exactement comme `TokenSpelling(Expr.Op)` pour
`Binary`. La constante `IndexOperator` existe déjà
(`TypeChecker/MemberResolver.hpp:10`) et devient partagée. La branche
`Frontend::Index` de `ExprInferencer.cpp:372-381` est **supprimée** : elle
devient morte, et c'est ce qui referme T4 (le typage passe par `CallType`,
donc par `CheckCallArgs`, donc l'index d'un `Array<T>` rend `T`).

Couverture : `samples/Sema/IndexLowering.vl` (lecture, écriture, compound,
`Hash` et `String`), `samples/Sema/IndexCompoundReceiver.vl`
(`VOLT_CHECK_EXPECT_FAIL`).

### B.3 — `DotCall` → `Call` (order 23, corrige T6-silence)

Après B.1, un `DotCall` ne peut plus provenir d'une chaîne coupée. Il reste
deux origines : le motif `when .even?` (déjà traité par `CaseLowering`,
order 22 — d'où l'order **23**, juste après) et un `.method` en position
d'instruction, qui signifie « receveur implicite `self` ».

Passe `DotCallLowering` : tout `DotCall` survivant devient
`Call( Member( SelfExpr, Method ), Args, ArgNames )`. C'est exactement le
choix déjà fait par `CaseLowering.cpp:59-67` pour un `case` sans cible :
cohérence, pas d'invention.

Complément indispensable côté `TypeChecker` : quand `Context.SelfValue` est
invalide (portée module), l'appel doit désormais diagnostiquer
`"'.method' has no implicit receiver here"` au lieu de retourner en silence
(`ExprInferencer.cpp:518-521` retourne aujourd'hui sans rapport si
`Values.Has(SelfValue)` est faux). La branche `Frontend::DotCall` de
`ExprInferencer.cpp` est ensuite **supprimée**.

Couverture : `samples/Sema/ImplicitSelfCall.vl`,
`samples/Sema/DotCallNoReceiver.vl` (`VOLT_CHECK_EXPECT_FAIL`).

### B.4 — `Binary`/`Unary` non primitifs → `Call`

Une passe ne peut pas décider seule : il faut le `LayoutKind` du receveur,
donc le type. Deux options, une seule est sans dette :

- ❌ passe post-`TypeChecker` : devrait annoter à la main les `UnitTypes` des
  `Call` qu'elle crée.
- ✅ **ne pas créer de nœud du tout.** `MemberType` résout déjà l'opérateur et
  écrit sa `Resolution` dans `Context.CalleeResolution[Id.Value]` — pour
  `Binary` comme pour `Call`. Le backend lit donc, pour un `Binary` :
  `CalleeResolution` présent **et** layout non primitif → appel de méthode ;
  layout primitif → instruction. Zéro nœud, zéro passe, zéro réimplémentation.

**Livrable B.4 :** aucune passe. Vérifier que `MemberType` peuple bien
`CalleeResolution` sur la branche `Binary`/`Unary` (aujourd'hui la valeur de
retour est utilisée mais l'écriture de la résolution doit être confirmée), et
**documenter le contrat dans `rules/`** — c'est un contrat backend, pas un
abaissement. Test : golden `check --output` exposant la résolution d'un
`"a" + "b"`.

### B.5 — `Interp` → concaténation (order 25, corrige T5)

Passe `InterpLowering`. `"a#{x}b"` devient :

```
Binary( +, Binary( +, StringLiteral"a", Call( Member( x, "to_string" ), [] ) ), StringLiteral"b" )
```

Règles :
- un `Part` qui est déjà un `StringLiteral` est concaténé tel quel (pas de
  `.to_string`) ;
- un `Interp` à une seule part non littérale devient le seul
  `Call(Member(part,"to_string"))` — le type reste `String` ;
- l'association est à gauche, ordre source strict.

`"to_string"` est un **nom de méthode**, pas un nom de type : c'est le
précédent déjà posé et commenté par `JsxLowering.cpp:33-38` (« Names only —
never a Volt type — so the zero-hardcode guard stays satisfied »), et par
`IndexOperator`. Le garde `tests/ZeroHardcode.cmake` reste vert.

> **Alternative écartée :** une annotation `@[Stringify]` (sur le patron de
> `@[Apply]`). Elle obligerait la passe à connaître le type de chaque part
> pour retrouver le membre annoté → passe post-`TypeChecker` → annotation
> manuelle des types → dette. Le nom fixe est ici le choix *moins* endetté.

Order 25 : après `MacroExpansion`(15)/`MagicExpansion`(16), car
`samples/Syntax/Macros/Serializable.vl` génère du texte contenant `#{...}`
qui est reparsé à l'order 15.

**B.5.bis — Le corollaire stdlib.** L'abaissement ne vaut que si `to_string`
existe. Aujourd'hui : présent sur `String`, `Bool`, `Exception` uniquement.
À écrire dans `source/Lib/` :

- `mixin Stringable` (`source/Lib/Mixins/Stringable.vl`) : `to_string` par
  boucle de chiffres (`/ 10`, `% 10`, signe), sur `Pointer<UInt8>.malloc` —
  le même patron que `String#trim`. Un seul algorithme, `self` polymorphe.
- `include Stringable` sur `Int8/16/32/64` et `UInt8/16/32/64`.
- `Char#to_string` (String d'un octet), `Float32/64#to_string` (partie
  entière + 6 décimales fixes), `Symbol#to_string`.
- **Hors périmètre, assumé :** `Array<T>#to_string` et `Hash<K,V>#to_string`
  demandent une contrainte « `T` est Stringable » que Volt n'a pas (pas de
  bornes sur génériques). `"#{arr}"` produira le diagnostic explicite
  `type Array has no member 'to_string'`. Un refus net n'est pas de la dette ;
  un silence en serait.

Couverture : `samples/Sema/StringInterp.vl` (littéral+expr, expr seule, entier,
flottant, booléen, imbrication, `#{}` dans un corps de macro),
`samples/Sema/InterpNoToString.vl` (`VOLT_CHECK_EXPECT_FAIL`).

**Validation B :** recensement §0.1 → `Interp` ×0, `Index` ×0, `DotCall` ×0,
`Section` ×0. `volt check source/Lib/` toujours 0 erreur.

---

## 4. Phase C — Totalité du typage (corrige T3)

C'est la phase la plus risquée et la plus rentable : elle va allumer des
erreurs partout, y compris probablement dans `source/Lib/`. À budgéter comme
telle, et à faire **après** B (un `Index` non typé produirait des faux
positifs en cascade).

**C.1 — Un prédicat d'assignabilité unique.** `ArgTypeMatches`
(`MemberResolver.cpp`) sait déjà comparer deux `SemaTypeId` en tolérant les
trous génériques non instanciés (le correctif `Pointer<Void>` de l'Étape A).
Le promouvoir en `Sema/Private/Passes/TypeChecker/TypeCompat.{hpp,cpp}` :
`IsAssignable( Context, SemaTypeId Target, SemaTypeId Value )`. **Un seul
prédicat**, sinon deux sites se contrediront — même leçon que
`IsBuiltinOpOn`.

**C.2 — Le câbler sur les cinq sites muets**, chacun avec le même diagnostic
(`"cannot assign X to Y"` / `"cannot return X from a method declared Y"`) :

1. `LocalDecl` porteur d'un `DeclType` → vs le type de `Init`.
2. `Assign` dont la cible a un type connu (local, `InstanceVar`, `Member`).
3. `Return` → vs `CurrentMethodReturnType`.
4. Expression finale d'un corps de méthode → idem (`TrailingType`).
5. Défaut de paramètre (`Param::Default`) → vs le type déclaré du paramètre.

Chaque site appelle **d'abord** `ConstrainExprType` (la propagation descendante
existante), **puis** `IsAssignable` — sinon `a : UInt64 = 8` casserait, le
littéral n'ayant pas encore été narrowé.

**C.3 — Absorber les retombées.** Estimation : `source/Lib/` va révéler des
signatures réellement fausses (candidats repérés à l'audit :
`Array#pop -> T` qui ne retourne rien quand `@size == 0` ;
`Hash#[] -> V` idem ; `Exception.vl:41` `symbols != null`). Chaque erreur est
soit un vrai bug stdlib à corriger, soit un manque du prédicat. **Aucune ne
doit être silencée par un assouplissement de `IsAssignable`** : c'est
exactement ainsi que la dette rentre.

**C.4 — `Nil` (T6).** Bloque `Exception.vl`. Décision recommandée, minimale :
`@[Literal( NilLiteral )] struct Nil` de layout `@[Primitive("ptr", 64)]`,
et `IsAssignable` accepte `Nil` vers tout layout `Pointer`. `NilableType`
(`T?`) reste **hors périmètre** et doit être refusé par un diagnostic explicite
(`"nilable types are not implemented"`) plutôt que d'être accepté sans
sémantique — c'est le seul point du plan que j'assume laisser ouvert, et il
doit l'être bruyamment.

Couverture : `samples/Sema/Assignability.vl` (les 5 sites valides),
`samples/Sema/AssignMismatch{Local,Assign,Return,Trailing,Default}.vl`
(5 `VOLT_CHECK_EXPECT_FAIL`).

---

## 5. Phase D — Le garde-fou (ce qui rend « 0 dette » structurel)

Sans cette phase, tout le reste est un instantané qui se dégrade au commit
suivant.

**D.1 — Marquer le sucre dans le manifeste.** `Frontend/AST/Nodes.inl` gagne
une seconde macro, sans toucher un seul consommateur existant :

```cpp
#ifndef VOLT_EXPR_SUGAR
    #define VOLT_EXPR_SUGAR( Name ) VOLT_EXPR( Name )
#endif
```

et les 9 nœuds du §1.2 passent de `VOLT_EXPR( Interp )` à
`VOLT_EXPR_SUGAR( Interp )`. Tous les consommateurs actuels (enum, variant,
LUT de noms) continuent de les voir ; seul qui définit `VOLT_EXPR_SUGAR`
distingue. Un futur nœud sucre = **une ligne**, conformément à
`rules/meta-first.md`.

**D.2 — Passe `AstInvariant` (order 40, `Analysis`).** Une ligne dans
`PassList.inl`, une fonction. Deux vérifications :

- **Sucre résiduel** : tout `ExprId` de l'arène dont le kind est dans
  l'ensemble `VOLT_EXPR_SUGAR` → diagnostic *erreur* nommant le nœud et sa
  `SourceRange`. Pas de `switch` : membership sur un `constexpr` généré par
  le manifeste.
- **Complétude du typage** : tout `ExprId` en **position de valeur** doit
  vérifier `Values.Has(Id)`. Positions de valeur : `Init` d'un `LocalDecl`,
  `Value` d'un `Assign`, `Value` d'un `Return`, `Args`/`BlockArg` d'un `Call`,
  opérandes de `Binary`/`Unary`/`Ternary`/`Deref`, parts d'un `ArrayLit`/
  `HashLit`. (Un `Member` en position de callee ou un `Identifier` en position
  de type n'en sont pas.)

**D.3 — Déploiement en deux temps, honnête.** La seconde vérification va
compter des dizaines de nœuds avant la fin de C. Donc :

- d'abord `PassStats::{ResidualSugarNodes, UntypedValueExprs}`, remontés par
  `volt check --metrics` — le compteur est visible, décroissant, non bloquant ;
- ensuite, **quand les deux comptent 0 sur `samples/**` + `source/Lib/`**,
  promotion en erreur dure et ajout d'un test CTest
  `VoltAstInvariant` qui rejoue le recensement sur tout l'arbre.

La bascule de non-bloquant à bloquant est le critère de merge du chantier,
pas une étape optionnelle.

**D.4 — `MacroDef` résiduels.** `MacroExpansion` laisse les `MacroDef` dans
`TopDecls` (×2 mesurés). Ils ne portent aucun code. Les retirer de `TopDecls`
en fin de passe (le nœud reste dans l'arène, seule la racine est nettoyée) —
2 lignes, et l'invariant D.2 les couvre ensuite.

---

## 6. Ordre d'exécution et invariants de bout en bout

Chaque phase est mergeable seule et laisse la suite verte.

| # | Phase | Pourquoi cet ordre |
|---|---|---|
| 1 | **A** — sûreté des réécritures | tant que T1 vit, aucun résultat de B n'est reproductible |
| 2 | **B.1** — point en tête | sinon B.3 grave un contresens |
| 3 | **B.2 / B.3 / B.5** — Index, DotCall, Interp | referment T4, T5 au passage |
| 4 | **B.4** — contrat opérateurs non primitifs | doc + vérif, pas de code |
| 5 | **C** — assignabilité + Nil | a besoin d'un AST déjà abaissé |
| 6 | **D** — invariant, en compteur puis en erreur | verrouille 1→5 |
| 7 | **§7** — règles, graphe, suppression des PLAN | clôture |

`PassList.inl` final :

```
FunctionalLowering  8   Lowering
PipelineLowering    9   Lowering
ScopeResolver      10   Analysis
EnumLowering       12   Lowering
MacroExpansion     15   Lowering
MagicExpansion     16   Lowering
JsxLowering        20   Lowering
CaseLowering       22   Lowering
DotCallLowering    23   Lowering   ← nouveau (après CaseLowering)
IndexLowering      24   Lowering   ← nouveau
InterpLowering     25   Lowering   ← nouveau
TypeChecker        30   Analysis
UnusedChecker      35   Analysis
AstInvariant       40   Analysis   ← nouveau
```

**Invariant structurel du plan : aucune passe ne s'exécute après
`TypeChecker`, sauf des passes `Analysis` qui ne créent aucun nœud.** C'est
ce qui garantit que tout nœud vu par un backend a un type. Toute future
proposition de « passe d'abaissement post-typage » doit d'abord expliquer
comment elle type ce qu'elle crée.

---

## 7. Définition de « terminé » (critères de merge)

Le frontend + middle-end sont déclarés finis quand **tous** ces points sont
vrais simultanément :

1. `volt-build format test` vert, `-Werror`, `volt-build tidy` propre.
2. `volt check source/Lib/` : 0 erreur.
3. `AstInvariant` est **bloquant** et compte 0 sucre résiduel + 0 expression
   de valeur non typée sur `samples/**` et `source/Lib/**`.
4. Le recensement §0.1 ne rend plus aucun nœud de la liste §1.2.
5. `volt-build debug asan` **et** `tsan` sur un circuit multi-fichiers :
   aucun rapport.
6. `tests/ZeroHardcode.cmake` vert.
7. Les 5 golden d'échec de la phase C et les 3 de la phase B sont en place
   (`VOLT_CHECK_EXPECT_FAIL`).
8. `graphify update .` passé, `graphify-out/GRAPH_REPORT.md` à jour.
9. Écrit dans `.agents/rules/` — **ce qui survit aux PLAN** :
   - `ast-rewrite.md` (A.3) : l'idiome de réécriture, avec le contre-exemple ;
   - `core-ast.md` : le tableau §1.1/§1.2 + les trois arbitrages §1.3, comme
     contrat d'entrée des backends ;
   - un paragraphe dans `meta-first.md` sur `VOLT_EXPR_SUGAR`.
10. `.agents/PLAN.md` et `.agents/PLAN_FRONTEND_100.md` supprimés.

Après quoi il ne reste plus que les 3 backends, dont l'entrée est un AST
noyau de 24 nœuds, tous typés, avec `CalleeResolution` et `ClosureEnvFrame`
déjà calculés.

---

## 8. Ce que ce plan n'inclut pas (dit explicitement)

- **`T?` / `NilableType`** : refusé par diagnostic, pas implémenté (§C.4).
  Nécessite un modèle de types somme que Volt n'a pas.
- **`Array#to_string` / `Hash#to_string`** : nécessitent des bornes sur
  génériques (§B.5.bis).
- **Lambda à paramètres non annotés sans type attendu** (`add10 = (&.+ 10)`
  puis `add10(5)`) : déjà identifié dans `PLAN.md §VI.5`, rejeté proprement
  par diagnostic, non résolu. Demande un solveur bidirectionnel — chantier
  séparé, sans impact sur les backends.
- **Monomorphisation des génériques** : c'est du codegen, donc backend.
