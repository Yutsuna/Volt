# ON_GOING — Mixins & méthodes d'ordre supérieur (dernière étape du MiddleEnd)

**Branche :** `Feat/Finish-Frontend`
**Dernier point de contrôle :** phases 0 à 8 **terminées et validées**. Le chantier est clos ; ce qui suit sert de mémoire et liste les questions restées ouvertes.
**État build/tests :** `volt-build format test` **110/110**, `volt-build tidy` propre, `volt check source/Lib/` **0 erreur**.

> **Convention de travail (importante).** Ne pas `git commit` de sa propre initiative. Le chantier se laisse dans le working tree ; c'est l'utilisateur qui découpe et rédige ses commits.

---

## 1. Le problème, en une page

L'objectif était celui de `PLAN.md §VII.1` : que `Array.vl` reçoive `.map()` / `.filter()` du mixin `Enumerable`. L'injection des membres de mixin *fonctionnait déjà* (`TypeStore::LookupMember` traverse corps propre → `Includes` → `Super`). Ce qui manquait était en amont et en aval, en **quatre trous silencieux** :

| # | Trou | État |
|---|---|---|
| 1 | le type `self` d'une signature ne résolvait pas → tout `Comparable`/`Arithmetic` renvoyait un type invalide, ce qui désarmait le diagnostic de membre inconnu | **CORRIGÉ (phase 1)** |
| 2 | aucun type stdlib ne réclamait `FuncType`/`Lambda`/`Block` → lambdas et blocs non typés | **CORRIGÉ (phase 0)** |
| 3 | les arguments génériques d'`include` sont jetés (`Includes` est un `SmallVec<NominalId>`) et `LookupOn` n'applique les args du receveur que si `Owner == Base` | **CORRIGÉ (phase 2)** |
| 4 | les paramètres d'un `do \| x \|` n'héritent pas du `&block : T -> Void` déclaré | **CORRIGÉ (phase 3)** |

Plus un manque de langage : `Frontend::Method` n'avait pas de `Generics`, donc `def map<U>( &block : T -> U ) -> Array<U>` était inexprimable — **CORRIGÉ (phase 4)**.

### Sonde de référence

Fichier jetable + `volt check <fichier>`. **Lancer depuis la racine du dépôt** (voir §5, la découverte de la stdlib est relative au CWD).

```volt
def probe -> Void
  arr = [ 1, 2, 3 ]
  arr.each do | i |
    i.no_such                        # ✓ type Int32 has no member 'no_such'
  end
  m = arr.map do | i | i > 1 end
  m.pop.also_no_such                 # ✓ type Bool has no member 'also_no_such'
  f = arr.filter do | i | i > 1 end
  f.pop.nope_either                  # ✓ type Int32 has no member 'nope_either'
end
```

---

## 2. Ce qui a été fait

Les phases 0 et 1 sont décrites dans l'historique git (commits jusqu'à `ddfe2fa`), les phases 2 à 4 dans les commits `191f2c3` → `63d55dd`.

### Phase 2 — `include Mixin<Args>` et lookup instancié

**Stockage.** `NominalType::Super` et `NominalType::Includes` passent de `NominalId` à **`SigTypeId`** (`TypeStore.hpp`) : l'`include` est gardé *tel qu'écrit*, dans l'espace de paramètres de l'incluant. `include Enumerable<T>` sur `Array<T>` ⇒ `SigType{ Base = Enumerable, Args = [ ParamIndex 0 ] }`. `TypeBinder.cpp` : `NominalOf` est remplacé par **`ParentOf`**, qui renvoie le `ResolveTypeExpr` complet, utilisé pour `Super` *et* `Include`.

**Deux helpers sur `TypeStore`**, pour que les deux traversées ne puissent pas diverger : `OwnMember( NominalId, Name )` (le corps propre seul, base de `LookupMember`) et `BaseOf( SigTypeId )`.

**Lookup instancié** — `LookupMemberOn` (`TypeResolve.hpp/.cpp`) : corps propre → chaque `Includes[i]` instancié contre les args du receveur → `Super`, profondeur bornée à 16, renvoyant un `InstantiatedMember{ Decl, Owner }` dont l'`Owner` est déjà concret. **`Self` reste toujours le receveur d'origine**, jamais le mixin traversé — c'est ce qui rend `Comparable#<( other : self )` correct sur `Int32`.

> ⚠️ **Piège d'invalidation mémoire.** `Instantiate` interne dans `Values` et peut déplacer l'arène. `LookupMemberOn` **copie** `Values.Get( Receiver )` avant d'instancier ; `LookupOn` copie de même. Ne pas « optimiser » ces copies en références.

**Correctif attrapé au passage — `T.new`.** `new` retombait sur `initialize`, dont le résultat déclaré est le `Void` que tout initialiseur écrit. `LookupOn` marque `bConstructor` et substitue le receveur comme résultat ; `Array<U>.new` en dépend. Constantes `ConstructorCall = "new"` / `ConstructorName = "initialize"` (`MemberResolver.hpp`) — de la syntaxe Volt, pas des noms de type.

### Phase 3 — typage descendant des blocs

`Resolution` gagne `SemaTypeId BlockParam` (le slot `&block` instancié, capturé au lieu d'être jeté) et `TypeCheckerContext` gagne `SemaTypeId ExpectedClosure`. `CallType` est réordonné : callee d'abord (c'est lui qui remplit `CalleeResolution`), puis les arguments, puis le `BlockArg` sous `ExpectedClosure = Found.BlockParam`. `BindClosureParams` consomme `ExpectedClosure` — un paramètre sans annotation prend `Expected.Args[ 1 + Index ]` (`Args[0]` est le résultat) — et **le remet à invalide immédiatement après lecture**, pour qu'une closure imbriquée ne le récupère pas par accident.

### Phase 4 — génériques au niveau méthode (`def map<U>`)

**Frontend :** `Frontend::Method` gagne `SymbolList Generics;`, rempli par `ParseMethod` **après** le nom et **avant** le `LParen` (sans danger pour `def <( … )` / `def <=>`). Les goldens existants n'ont pas bougé : le dumper réflexif n'émet pas une `SymbolList` vide.

**Sema :** `SignatureResolver` construit un span `Space = [ génériques du type…, génériques de la méthode… ]` et résout toute la signature contre lui ; `Member::OwnGenerics` porte la coupure. **`UnifySig`** fait un parcours parallèle motif/type concret : un `ParamIndex` non lié se lie, un slot déjà lié gagne (un mauvais argument reste donc un diagnostic au lieu de redéfinir `T` en silence), `self` n'apprend rien, une base nominale différente arrête la descente.

**Le flux d'inférence** : `Resolution` gagne `Bindings` (args du propriétaire ++ un trou par générique de méthode) et `Receiver`. `Reinstantiate` recalcule `Result`/`Params`/`BlockParam` depuis `Bindings` (idempotent) ; `UnifyArgs` lie depuis les arguments positionnels en sautant le slot `&block` **exactement comme `Reinstantiate`** ; `UnifyBlock` lie depuis le type réel du bloc. `CallType` orchestre et **retourne `Found.Result`, pas la valeur d'origine du callee** (qui avait encore ses trous ouverts).

### Phase 5 — conformité des `abstract def`

- `Member::bAbstract` (`TypeStore.hpp`), rempli par `DeclareMembers`.
- `DeclStmtWalker.cpp`, namespace anonyme : `CollectIncludes` (tous les mixins atteignables par `include`, transitivement, sans doublon — **le `Super` n'est délibérément pas suivi** : ce qu'il inclut, il a lui-même été tenu de l'implémenter) et `CheckAbstractConformance` (re-résolution depuis `Context.SelfValue` via `LookupMemberOn`, exigeant une déclaration non abstraite ; une implémentation héritée ou fournie par un autre mixin compte).
- `EnterType( …, Loc, bConcrete )` : `WalkDecl` passe `true` pour `Struct`/`Class`, **`false` pour `Mixin`** — un mixin a le droit de faire suivre un contrat.
- **`IsBuiltinOpOn( Context, Base, Name )`** (`MemberResolver.hpp/.cpp`) remplace `IsBuiltinPrimitiveOp` : layout `Primitive`/`Pointer` **et** nom d'opérateur. Consommé par `MemberType` *et* par `CheckAbstractConformance`, ce qui lève les 66 non-conformités de `mixin Arithmetic` sur les primitifs sans affaiblir le contrôle sur un `struct` ordinaire. Le prédicat de « nom d'opérateur » est purement lexical (ne commence ni par une lettre ni par `_`, plus `and`/`or`/`not`) : aucun opérateur n'y est listé à la main. Voir `rules/zero-hardcode.md`, section « Primitive operators ».
- **`Core::SourceRange::Head()`** (`Core/Diagnostics/SourceLocation.hpp` + `.inl`) : le range replié sur son premier octet. Le diagnostic de conformité l'utilise, sinon le caret soulignait les 200 caractères de la déclaration entière.

### Phase 6 — implicite `self` et stdlib `Enumerable<T>`

**Appels à receveur implicite (option 2 du plan, tranchée).** La branche `Identifier` de `ComputeExpr` (`ExprInferencer.cpp`) retombe désormais sur `LookupOn( Context, Context.SelfValue, Name )` quand le nom n'est ni un local ni un type, et remplit `CalleeResolution` comme le fait la branche `Member`. C'est ce qui rend `each do | item | … end` équivalent à `self.each do … end` — sans quoi les corps de `map`/`filter` n'auraient pas typé leurs blocs. **Elle reste silencieuse quand rien ne correspond** : un appel de fonction libre atterrit ici aussi, et le compilateur n'a pas encore de table de fonctions libres.

> ⚠️ **Effet de bord réel, corrigé dans la stdlib.** Un membre de `self` l'emporte maintenant sur un `external def` de même nom. `Pointer#free` faisait `free( self )` en visant le libc — c'était une récursion infinie latente. Les deux externals sont renommés `libc_malloc` / `libc_free`, avec le symbole C donné explicitement (`@[External( "libc", "free" )]`, forme à deux arguments documentée dans `rules/zero-hardcode.md`).

**`source/Lib/Mixins/Enumerable.vl`** est réécrit en `mixin Enumerable<T>` : `abstract def each`, puis `map<U>`, `filter`, `to_array`, `count`, `any?`, `all?`. Les anciens `any?`/`all?` déclaraient `&block : self -> Bool`, ce qui était faux (`self` est la collection, pas l'élément). `source/Lib/Primitives/Array.vl` fait `include Enumerable<T>`.

**Goldens et échantillons créés** sous `samples/Sema/` (auto-enregistrés par le glob de `cmake/VoltTests.cmake`, goldens sous `tests/golden/samples/Sema/`) :
`MixinGenerics.vl`, `BlockParamTypes.vl`, et `AbstractConformance.vl` — ce dernier ajouté à `VOLT_CHECK_EXPECT_FAIL`, il doit être **rejeté**.

---

### Phase 7 — convention d'appel `&` et callables

**`&` au call-site remplit le slot de bloc.** Le sigil garde son sens unique (« ceci est un callable », `ESectionKind::StaticCapture`) ; ce qui est nouveau est que, **en dernière position et sans nom d'argument**, il alimente `BlockArg` au lieu d'`Args`. Décidé au parse-time par `Parser::PromoteCapturedBlock` (`ParseExpr.cpp`), appelé aux deux sites de construction d'un `Call` — l'appel parenthésé et `ParseCommandCallArgs`. `DotCall` n'a pas de `BlockArg` et n'est pas concerné. Sema ne contient **aucune** règle : la structure de l'AST reflète ce qui a été écrit.

`&` accepte désormais `(` en plus d'`identifier`/`Constant`, ce qui débloque la forme inline `numbers.map( &( ( &.+ 10 ) >> ( &.* 2 ) ) )` en plus de la forme nommée `numbers.map( &transform )`.

> Le reste suit tout seul : `FunctionalLowering` (Order 8) réécrit déjà les `Section`/`Composition` en `Lambda` **par Id**, donc une section déposée dans `BlockArg` y devient une lambda sans que la passe ait à le savoir.

**`LookupApplyOn` — appeler une valeur.** `f( x )` où `f` est un local ne résolvait aucun membre, et `CallType` renvoyait alors le type du callee lui-même : `f( 2 )` valait `Proc`, pas `Bool`. `LookupApplyOn( Context, Receiver )` (`MemberResolver.hpp/.cpp`) cherche le membre que le type **annote `@[Apply]`** — par le drapeau `Member::bApply`, jamais par un nom — et re-résout par son nom pour repasser par le chemin d'instanciation normal. `CallType` l'appelle quand `CalleeResolution` est vide. Effet mesuré sur `samples/Functional/FunctionalSpec.vl` : **11 → 6 erreurs**.

---

### Phase 8 — diagnostic strict sur `&block` non inféré, et couverture de la phase 7

Le point 1 de l'ancien §3 (ci-dessous) n'était pas qu'une limite connue : c'était une **régression de diagnostic** introduite par la phase 7. Avant, `numbers.map( transform )` sortait une erreur d'arité (aucun `&block` fourni). Après la phase 7, `numbers.map( &transform )` avec `transform` non annoté passait le contrôle sans rien dire et rendait un `Array<?>`. Décision tranchée avec l'utilisateur : **diagnostic d'inférence**, pas de typage bidirectionnel inter-instructions (le second exigerait un solveur de contraintes complet — dette assumée, cf. point 1 du §3 actualisé).

**`IsBlockResultInferred`** (`ExprInferencer.cpp`, namespace anonyme), consommé par `CallType` juste après le typage de `Expr.BlockArg` sous `ExpectedClosure`. Ne vérifie **que `Args[0]`** (le `Result` du nominal `Lambda`/`Block`, cf. `ClosureType`), et **seulement quand `Expr.BlockArg` est un nœud `Frontend::Lambda`** — jamais un `Frontend::Block`. Les deux raisons sont liées :

- Un `do | x | … end` (`Block`) est une séquence d'instructions le plus souvent sans expression finale (`each do | item | … end` n'en type jamais), et ses paramètres sont légitimement invalides quand on type le **corps d'une définition générique** elle-même — `T` à l'intérieur de `Enumerable<T>` est un espace réservé non résolu (`UnitSink::Param` renvoie toujours `IdType{}`), pas un échec. Ni son `Result` ni ses paramètres ne sont un signal fiable ; `Block` est donc ignoré entièrement.
- Tout `&expr` est réécrit en `Lambda` par `FunctionalLowering` (`(fn_tmp) => expr(fn_tmp)`) **avant** que cette passe ne tourne (Order 8 < Order 30) — donc `&transform` comme `&.+ 10` arrivent tous deux en `Lambda` ici. Le paramètre `fn_tmp` d'une `Lambda` se résout toujours mécaniquement depuis le slot `Expected` de l'appelant, quel que soit `expr` — le vérifier aurait redéclenché le même faux positif que sur les corps génériques. Ce qui manque réellement quand `expr` (ex. une closure locale déclarée sans annotation) n'a jamais été résolue, c'est le **`Result`** : exactement ce qui alimente le `U` d'un `map<U>`, et exactement ce qui produisait `Array<?>` en silence.

Message : `cannot infer block parameter types for '<nom du membre appelé>' — please add explicit type annotations`. Vérifié qu'il **ne** se déclenche **pas** sur `Enumerable.vl` (ses six `each do | item | … end` internes), ni sur `&annotated`, ni sur `do | x | … end` littéral, ni sur le point-free inline `numbers.map( &( &.+ 10 ) )` — seulement sur `numbers.map( &transform )` avec `transform` non annoté.

**Deux trous de couverture fermés :**
- `samples/Sema/CallableArgs.vl` — valide `Parser::PromoteCapturedBlock` (un `&add_one` non nommé en dernière position migre vers `BlockArg`) et `LookupApplyOn` (`add_one( 10 )` et `add_one.call( 10 )` doivent typer pareil).
- `samples/Sema/BlockParamTypes.vl` — bloc à deux paramètres sur `items.each_with_index do | item, index | … end`, verrouillant l'arité multiple de bloc (seul point que la phase 3 n'avait jamais sondé).

Goldens régénérés (`cmake -DUPDATE=1 -P tests/GoldenTest.cmake`), `volt-build format test` **110/110**, `volt-build tidy` propre, `graphify update .` refait.

---

## 3. Ce qui reste ouvert

1. **Une lambda à paramètres non annotés, sans type attendu, reste non typée — par choix, pas par oubli.** C'est le blocage de fond du point-free, en amont de tout ce qui précède :
   ```volt
   add10 = ( &.+ 10 )     # lowering → ( fn_tmp ) => fn_tmp + 10
   add10( 5 )             # non inféré : rien ne donne son type à fn_tmp
   ```
   Un `&block` passé à `map` hérite bien de `ExpectedClosure`, mais une section **stockée dans un local** n'a pas de type attendu au moment de sa propre définition. Depuis la phase 8, le cas `numbers.map( &transform )` avec `transform` non annoté **ne compile plus silencieusement** — il est rejeté avec un diagnostic explicite au lieu de rendre un `Array<?>` fantôme. Le combler pour de vrai (faire marcher `add10( 5 )`, ou accepter `numbers.map( &transform )`) demande soit d'annoter (`( x : Int32 ) => …` marche déjà), soit une inférence depuis le site d'usage — un vrai chantier de solveur bidirectionnel, pas un correctif.
2. **`FunctionalSpec.vl` — 6 erreurs restantes.** `:50` attend maintenant le `&` (`numbers.map( &( … ) )`) ; les quatre `Proc has no member '=='` restants passent par `|>` (`2 |> calc_pointfree == 24`) et relèvent du point 1 ; `String` n'a toujours pas de `trim`.
3. **Pas de table de fonctions libres.** `TypeStore` ne connaît que des types nominaux ; un appel de fonction libre ne résout donc rien et ne diagnostique rien. C'est ce qui oblige la branche `Identifier` à rester silencieuse en dernier recours.
4. **`&` est devenu positionnellement significatif.** Une méthode qui voudrait un `Proc` positionnel *en dernière position* et un bloc n'a plus d'échappatoire — sauf à nommer l'argument (`f( fn: &g )`, qui n'est pas promu). Dette assumée.
5. **`numbers.map( &.+ 10 )` sans composition ni parenthèses supplémentaires échoue en arité** (`map takes 0 argument(s), but 1 were given`), alors que `numbers.map( ( &.+ 10 ) >> ( &.* 2 ) )` (composition) et `numbers.map( transform )` (local nommé) fonctionnent. Repéré en sondant la phase 8, non investigué — pas dans le périmètre du diagnostic ; probablement une interaction de précédence Pratt entre `&` et `.` dans `ParseCommandCallArgs`/l'appel parenthésé.

---

## 4. Clôture (faite)

```sh
volt-build format test          # 110/110
volt-build tidy                 # propre
volt check source/Lib/          # 0 erreur
graphify update .               # TypeResolve, Method::Generics, IsBuiltinOpOn, SourceRange::Head, IsBlockResultInferred
```

Regénération des goldens (`volt-build` n'a pas de tâche dédiée pour ça — c'est le seul appel `cmake` direct toléré, documenté dans la skill `format-and-check`) :

```sh
cmake -DUPDATE=1 -P tests/GoldenTest.cmake
```

`PLAN.md` est à jour : §VI items 2 et 5 → FAIT, §VII.1 → FAIT, la prochaine étape est §VII.2 (codegen / `volt run`).

---

## 5. Pièges rencontrés (à ne pas re-découvrir)

- **`volt-build` ne recompile pas toujours** après une salve d'édits : `ninja: no work to do` alors que les sources ont changé — et le symptôme peut être un **échec de link fantôme** (`FAILED: lib/libSema_d.so`) suivi d'un `no work to do` au run suivant. `touch source/Volt/Sema/Private/Passes/TypeChecker/*.cpp source/Volt/Sema/Private/Layout/*.cpp` puis relancer. Toujours vérifier qu'une ligne `Building CXX object …` apparaît avant de croire un résultat de `volt check`.
- **`volt check` découvre la stdlib relativement au CWD.** Lancé depuis un autre répertoire, il ne charge que le fichier cible (« 1 file(s) ») et sort des erreurs absurdes du genre *no type claims IntLiteral*. **Toujours lancer depuis la racine du dépôt**, même avec un chemin absolu en argument.
- **Le binaire de test est `build/debug-testing/bin/Volt_d`** (pas `build/bin/`, où `GoldenTest.cmake` va le chercher par défaut — d'où le `-DVOLT_BIN=` ci-dessus).
- **Le `Void` de Volt n'existe pas** comme type déclaré dans `source/Lib/` : `-> Void` résout vers un id invalide. Toléré partout (un id invalide veut dire « non inféré », jamais une erreur). L'ajouter serait un changement séparé, avec un risque de nouveaux diagnostics dans la stdlib.
- **Invalidation d'arène.** `Instantiate` / `ResolveTypeExpr` internent dans `UnitTypes` et peuvent déplacer l'arène : ne jamais garder une `const SemaType &` (ni une référence sur ses `Args`) à travers un de ces appels.
- **Un membre de `self` masque un `external def` de même nom** depuis la phase 6. Si un appel à un external se met à diagnostiquer une arité absurde, c'est ça — préfixer le nom Volt de l'external et donner le symbole C dans l'annotation.
- **Les diagnostics clang-tidy remontés par l'IDE** sur la pass `TypeChecker` (`readability-redundant-member-init`, `modernize-use-designated-initializers`, `misc-use-internal-linkage`, `modernize-use-ranges`) sont **préexistants ou hors configuration** appliquée par `volt-build tidy`. Ne pas les traiter comme des régressions.
- **`-Wmissing-designated-field-initializers` est fatal** (`-Werror`). Toute construction de `Resolution{ .Decl = …, … }` doit lister **tous** les champs.
- **ZeroHardcode (`tests/ZeroHardcode.cmake`) scanne aussi les identifiants C++**, pas seulement les chaînes : la liste inclut `Char`, `Int`, `Bool`, `Hash`, `Nil`… Une variable locale nommée `Char` fait échouer le test.
- **Un `SemaTypeId` invalide est un sentinel surchargé** (phase 8) : `UnitSink::Param` (`TypeResolve.hpp`) renvoie `IdType{}` aussi bien pour « générique non substitué, en train de typer la définition elle-même » (`T` dans le corps de `Enumerable<T>`) que pour « inférence réellement échouée ». Les deux sont indiscernables par simple `IsValid()`. Toute nouvelle vérification de validité sur un type dérivé d'un corps potentiellement générique doit soit se limiter à un slot qui ne peut *que* venir d'un échec réel (ici : `Result` d'une `Lambda`, jamais ses paramètres, et jamais un `Block`), soit être explicitement désactivée à l'intérieur d'un corps générique — sinon chaque appel interne d'un mixin/type générique à lui-même redevient un faux positif.
- **`volt-build` est déjà sur le PATH de cette session** — pas besoin de `nix develop --command volt-build …`. L'appeler nu.
