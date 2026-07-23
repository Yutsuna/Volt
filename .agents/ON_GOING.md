# ON_GOING — Mixins & méthodes d'ordre supérieur (dernière étape du MiddleEnd)

**Branche :** `Feat/Finish-Frontend`
**Dernier point de contrôle :** phases 0 et 1 terminées, build vert, **98/98 tests CTest**,
`volt check source/Lib/` OK sur 15 fichiers.
**Non commité :** les modifications décrites en §2 sont dans le *working tree*.

Ce document est autonome : il suffit à reprendre le chantier dans une nouvelle session sans
relire l'historique de conversation. Le plan d'origine est dans
`~/.claude/plans/sprightly-orbiting-clock.md`, mais **ce fichier fait foi** — il intègre les
écarts découverts pendant l'implémentation.

---

## 1. Le problème, en une page

L'objectif est celui de `PLAN.md §VII.1` : que `Array.vl` reçoive `.map()` / `.filter()` du
mixin `Enumerable`. L'injection des membres de mixin *fonctionnait déjà*
(`TypeStore::LookupMember` traverse corps propre → `Includes` → `Super`). Ce qui manquait est
en amont et en aval, en **quatre trous silencieux** :

| # | Trou | État |
|---|---|---|
| 1 | le type `self` d'une signature ne résolvait pas → tout `Comparable`/`Arithmetic` renvoyait un type invalide, ce qui **désarmait** le diagnostic de membre inconnu | ✅ **CORRIGÉ (phase 1)** |
| 2 | aucun type stdlib ne réclamait `FuncType`/`Lambda`/`Block` → lambdas et blocs non typés | ✅ **CORRIGÉ (phase 0)** |
| 3 | les arguments génériques d'`include` sont jetés (`Includes` est un `SmallVec<NominalId>`) et `LookupOn` n'applique les args du receveur que si `Owner == Base` | ❌ **À FAIRE (phase 2)** |
| 4 | les paramètres d'un `do \| x \|` n'héritent pas du `&block : T -> Void` déclaré | ❌ **À FAIRE (phase 3)** |

Plus un manque de langage : `Frontend::Method` n'a pas de `Generics`, donc
`def map<U>( &block : T -> U ) -> Array<U>` est inexprimable (**phase 4**).

### Sondes de référence

Fichier jetable + `volt check <fichier>`. C'est le moyen le plus rapide de mesurer l'avancement.

```volt
def probe -> Void
  a = 3
  x = a.min( "oops" )              # phase 1 → argument 1 to min has type String, expected Int32  ✅
  y = a.abs.nonexistent_method     # phase 1 → type Int32 has no member 'nonexistent_method'      ✅
  arr = [ 1, 2, 3 ]
  arr.each do | i |
    i.no_such                      # phase 3 → doit devenir une erreur (silence aujourd'hui)
  end
  arr.map do | i | i > 1 end       # phase 4/6 → doit valoir Array<Bool>
end
```

---

## 2. Ce qui est déjà fait (working tree, non commité)

```
?? source/Lib/Primitives/Proc.vl
 M source/Volt/Frontend/Public/Volt/Frontend/AST/Type.hpp
 M source/Volt/Sema/Private/Layout/TypeBinder.cpp
 M source/Volt/Sema/Private/Layout/TypeResolve.cpp
 M source/Volt/Sema/Private/Passes/TypeChecker.cpp
 M source/Volt/Sema/Public/Volt/Sema/Layout/TypeResolve.hpp
 M source/Volt/Sema/Public/Volt/Sema/Layout/TypeStore.hpp
```

### Phase 0 — le type stdlib `Proc`

- **`source/Lib/Primitives/Proc.vl` (neuf)**
  ```volt
  @[Literal( FuncType ), Literal( Lambda ), Literal( Block )]
  struct Proc<R>
    @[Apply]
    abstract def call -> R
  end
  ```

- **`Type.hpp` — `FuncType` : `Return` passe *avant* `Params`.** L'ordre des champs réflexifs
  *est* l'ordre des arguments génériques du type qui réclame le nœud. Un callable a exactement
  un résultat mais un nombre quelconque de paramètres : seul un résultat en tête occupe un
  indice fixe qu'une signature peut nommer (`-> R` dans `Proc<R>`). Convention finale :
  **`Proc< Return, Params... >`**.

- **`TypeBinder.cpp` — `@[Literal]` liable plusieurs fois.** La liaison `BindNodeKind` se fait
  désormais *dans* la boucle d'annotations (avant : une variable `NodeKind` écrasée puis liée
  après la boucle), ce qui permet à un type de réclamer plusieurs nœuds en répétant
  l'annotation. `SetLiteralSlots` n'est appelé que si l'annotation porte des arguments
  supplémentaires, pour qu'une annotation sœur n'efface pas les slots.

- **`@[Apply]` — nouvelle annotation stdlib.** Problème résolu : `Proc` a une arité portée par
  son *type*, pas par la déclaration de `call`, donc `block.call( item )` échouait sur
  « call takes 0 argument(s) ». `@[Apply]` sur une méthode dit : *la signature de ce membre est
  la liste des arguments du receveur — résultat d'abord, puis paramètres*.
  - `Member::bApply` (`TypeStore.hpp`), rempli par `DeclareMembers` (`TypeBinder.cpp`), qui
    accumule maintenant les `Frontend::Annotation` d'un corps de type comme le fait déjà
    `ForEachTypeDecl` au niveau fichier.
  - Consommé en tête de `LookupOn` (`TypeChecker.cpp:546`).
  - C'est la façon zero-hardcode d'invoquer un callable : le C++ ne sait pas ce qu'est un
    callable, c'est le type stdlib qui marque son propre `call`.

- **`TypeChecker.cpp` — closures.** L'ancienne branche `Lambda` (une centaine de lignes, avec
  un fallback sur `PointerType` et une branche « captures comme arguments de type ») est
  remplacée par trois helpers + deux branches courtes :
  - `BindClosureParams( ParamList )` (l.666) — lie les paramètres comme locaux, renvoie leurs types.
  - `TrailingType( StmtList )` (l.689) — un corps de statements vaut son expression finale (pour `Block`).
  - `ClosureType( NodeKind, Result, Params )` (l.713) — `Proc< Result, Params... >`.
  - branches `Frontend::Lambda` (l.874) et `Frontend::Block` (l.887).
  - ⚠️ **Changement sémantique volontaire :** le type d'une closure capturante n'encode plus
    ses captures. Un environnement est une préoccupation mémoire que `ClosureFrame` dérive de
    la `ScopeTable` ; ce n'est pas ce qu'*est* un callable. Le typage uniforme est ce qui
    permettra à la phase 4 d'unifier un bloc contre un `&block : T -> U`.

### Phase 1 — le type `self`

- **`TypeStore.hpp` :** `SigType::SelfParam = -2`, sentinelle à côté de `ParamIndex >= 0`.
  `self` n'est pas un type mais un type *différé* : celui du receveur.
- **`TypeResolve.hpp` :** `SigSink::SelfRef()` → `Param( SelfParam )` ;
  `UnitSink` gagne un champ `SemaTypeId Self` et `SelfRef()` le renvoie (un corps de méthode
  *connaît* son `self`, contrairement à une déclaration). Les 4 sites de construction dans
  `TypeChecker.cpp` passent `.Self = SelfValue`.
- **`ResolveTypeExpr` :** dans la branche `TypeRef`, un chemin d'un seul segment égal à
  `Frontend::TokenSpelling( Frontend::TokenKind::KwSelf )` renvoie `Out.SelfRef()`.
  **Zero-hardcode respecté** : `self` est un mot-clé (`TokenKind.inl:106`), pas un nom de type
  Volt, et l'orthographe est lue dans la table de tokens — aucun littéral `"self"` n'apparaît
  dans `Sema/`.
- **`Instantiate` prend un paramètre `SemaTypeId Self`** (`TypeResolve.hpp` / `.cpp`) et le
  renvoie pour la sentinelle. Les appels de `LookupOn` passent `Receiver`.

---

## 3. Ce qu'il reste à faire

### Phase 2 — `include Mixin<Args>` et lookup instancié  ← **PROCHAINE ÉTAPE**

**Pourquoi.** `Enumerable` doit devenir `Enumerable<T>` pour que `each` / `map` parlent du type
d'élément. Aujourd'hui `AddInclude` ne garde qu'un `NominalId` : le `<T>` serait perdu. Et
`LookupOn` (`TypeChecker.cpp:531-538`) n'applique les arguments du receveur que si le membre est
déclaré par le receveur lui-même — un membre venu d'un mixin est instancié avec un `Applied`
**vide**.

**Stockage.** `NominalType::Includes` : `SmallVec<NominalId,2>` → `SmallVec<SigTypeId,2>`. On
stocke l'`include` *tel qu'écrit*, dans l'espace de paramètres de l'incluant :
`include Enumerable<T>` sur `Array<T>` ⇒ `SigType{ Base = Enumerable, Args = [ ParamIndex 0 ] }`.
- `TypeStore::AddInclude` change de signature.
- `TypeBinder.cpp`, `SignatureResolver::Resolve`, branche `Frontend::Include` : utiliser
  `ResolveTypeExpr( …, SigSink, Entry.Target )` **complet** au lieu de `NominalOf`, qui ne
  gardait que la base. Faire la même chose pour `Super` (`class X < Y<T>`) — correctif gratuit.

**Lookup.** `TypeStore::LookupMember` ne peut pas composer les substitutions le long de la
chaîne (Array → Enumerable → …) : il est `const` et n'a pas d'`UnitTypes` pour interner.
Déplacer la traversée dans `TypeResolve.hpp/.cpp`, où `UnitTypes` est disponible :

```cpp
struct InstantiatedMember
{
    const Member *Decl = nullptr;
    SemaTypeId Owner;   // le type déclarant, DÉJÀ concret : Enumerable<Int32>
};

[[nodiscard]] SEMA_EXPORT InstantiatedMember
LookupMemberOn ( const TypeStore &Store, UnitTypes &Values, SemaTypeId Receiver,
                 SemaTypeId Self, std::string_view Name, std::uint32_t Depth = 0 );
```

Algorithme :
1. corps propre de `Values.Get( Receiver ).Base` → `{ Entry, Receiver }` ;
2. sinon, pour chaque `Includes[i]` :
   `Instantiate( Store, Includes[i], Values.Get(Receiver).Args, Self, Values )` donne le mixin
   **concret**, et on récurse dessus ;
3. puis `Super`, pareil ;
4. borne de profondeur 16 conservée (hiérarchie cyclique malformée).

⚠️ **`Self` reste toujours le receveur d'origine**, jamais le mixin traversé. C'est ce qui rend
`Comparable#<( other : self )` correct sur `Int32`.

Puis `TypeChecker::LookupOn` (l.510) s'y branche :
- supprimer la garde `if ( Found.Owner == Base )` ;
- `Applied` devient `Values.Get( Found.Owner ).Args` ;
- `Self` reste `Receiver`.

`TypeStore::LookupMember` reste utile pour l'existence de nom pure (aujourd'hui aucun autre
appelant : la mention dans `ScopeResolver.cpp:90` est un commentaire).

**Validation.** Mixin jouet :
```volt
mixin Box<T>
  abstract def get -> T
end
struct Holder
  include Box<Int32>
  def get -> Int32
    0
  end
end
```
puis `Holder.new.get.no_such` doit dire *type Int32 has no member*.

---

### Phase 3 — typage descendant des blocs

**Pourquoi.** `CallType` (`TypeChecker.cpp:897`) fait `InferExpr( Expr.BlockArg )` **avant**
de résoudre le callee, et sans type attendu. Les paramètres du `do | x |` n'héritent donc
jamais du `&block : T -> Void` déclaré, et `x` sort non typé.

Rappel de forme d'AST (vérifié via `volt parse`) : `arr.any? do | x | … end` est
`Call{ Callee = Member 'any?', BlockArg = Block }`. **`DotCall` n'a pas de `BlockArg`** — le
canal est toujours `Call::BlockArg`.

À faire :
1. `Resolution` (l.138) gagne `SemaTypeId BlockParam;` — le slot `&block` instancié. Aujourd'hui
   la boucle de `LookupOn` sur `ParamIsBlock` se contente de le **sauter** (l.564-568) ; le
   capturer au lieu de le jeter.
2. **Réordonner `CallType`** : callee d'abord (c'est lui qui remplit `CalleeResolution`), puis
   les arguments, puis le `BlockArg`. Le callee ne dépend pas des arguments, donc c'est sûr.
3. Poser un membre `SemaTypeId ExpectedClosure` avant `InferExpr( Expr.BlockArg )`, consommé par
   `BindClosureParams` : un paramètre sans annotation prend son type dans
   `Values.Get( ExpectedClosure ).Args[ 1 + i ]` (rappel : `Args[0]` est le résultat) au lieu
   d'un id invalide. Même discipline sauvegarde/restauration que `EnterMethod`.

---

### Phase 4 — génériques au niveau méthode (`def map<U>`)

**Frontend (~10 lignes) :**
- `Frontend::Method` (`AST/Decl.hpp:56`) : ajouter `SymbolList Generics;` après `Name`.
- `ParseDecl.cpp` : `Node.Generics = ParseGenericParams();` juste après la consommation du nom,
  en réutilisant l'helper existant (`ParseDecl.cpp:134`). Sans danger pour `def <( … )` /
  `def <=>` : le nom-opérateur est déjà consommé quand l'helper regarde le `<`.
- ⚠️ **Le nouveau champ apparaît dans le dump réflexif → regénérer les goldens.**
  Cible CMake `golden-update` (voir `tests/GoldenTest.cmake`, `-DUPDATE=1`). Relire le diff.
  Note : la réorganisation de `FuncType` (phase 0) n'a rien cassé car aucun golden ne contient
  de `FuncType` — ce ne sera pas le cas ici.

**Sema — espace de paramètres concaténé :**
- `SignatureResolver` (`TypeBinder.cpp`) construit pour un `Frontend::Method` un span
  `Generics = [ génériques du type…, génériques de la méthode… ]`. Les indices de la méthode
  commencent à `N` = nombre de génériques du type.
- `Member` gagne `std::uint32_t OwnGenerics = 0;` (la coupure).
- Nouvelle fonction libre dans `TypeResolve` :
  ```cpp
  void UnifySig ( const TypeStore &Store, const UnitTypes &Values, SigTypeId Pattern,
                  SemaTypeId Actual, std::span<SemaTypeId> Bindings );
  ```
  parcours parallèle ; un `ParamIndex >= N` non encore lié se lie à `Actual`.
- Dans `LookupOn` / `CheckCallArgs` : `Applied` = args du receveur ++ `OwnGenerics` trous ;
  unifier avec les args explicites (`GenericInst`), puis les args positionnels, puis typer le
  bloc (phase 3) et **unifier le `Proc` obtenu contre le `SigType` du `&block`** — c'est là
  que `U` se lie au type de retour du bloc. Ré-instancier `Result` une fois `Applied` complet.
- Un trou non résolu reste un id invalide, **sans diagnostic inventé** : c'est la discipline du
  reste du fichier.

---

### Phase 5 — conformité des `abstract def`

- Remonter `Frontend::Method::bAbstract` dans `Member` (nouveau `bool bAbstract`) depuis
  `DeclareMembers` (phase A de `TypeBinder`) — même endroit que `bApply`.
- Dans `TypeChecker`, à l'entrée d'un `Struct`/`Class` : pour chaque mixin inclus
  transitivement, tout membre `bAbstract` doit être re-résolu depuis le receveur et tomber sur
  une déclaration **non abstraite**. Sinon :
  `Report( Loc, "type X does not implement abstract member 'y' required by mixin Z" )`.
- ⚠️ Vérifier que `Proc#call` (abstrait, mais déclaré par `Proc` lui-même et non hérité) n'est
  pas signalé — le contrôle ne porte que sur les mixins *inclus*.

---

### Phase 6 — stdlib `Enumerable<T>`

Cible (`source/Lib/Mixins/Enumerable.vl`) :
```volt
mixin Enumerable<T>
  abstract def each( &block : T -> Void ) -> Void

  def map<U>( &block : T -> U ) -> Array<U>
  def filter( &block : T -> Bool ) -> Array<T>
  def each_with_index( &block : T -> Void ) -> Void
  def any?( &block : T -> Bool ) -> Bool     # aujourd'hui `self -> Bool`, ce qui est faux
  def all?( &block : T -> Bool ) -> Bool
end
```
et `source/Lib/Primitives/Array.vl:3` : `include Enumerable` → `include Enumerable<T>`.

Les autres mixins (`Comparable`, `Arithmetic`, `Hashable`) ne changent pas de forme — leur
`self` est simplement *résolu* depuis la phase 1.

**Risque à sonder AVANT d'écrire les 6 méthodes :** le corps de `map` a besoin de
`Array<U>.new` puis `.push` — un `GenericInst` utilisé comme valeur, chemin `GenericInstType` +
`LookupOn( …, "new" → "initialize" )`, présent mais jamais exercé sur un paramètre de méthode.
Écrire d'abord un extrait minimal et le passer à `volt check`.

---

## 4. Clôture

```sh
volt-build format tidy test     # -Werror ; 98/98 aujourd'hui
volt check source/Lib/          # doit rester 0 erreur
graphify update .               # Proc.vl + nouvelles fonctions libres de TypeResolve
```

Nouveaux goldens à créer sous `tests/golden/samples/Sema/` (format des `UnknownMember.vl.golden`
existants : un `.golden` + un `.lowered.golden`) :
`MixinGenerics.vl`, `BlockParamTypes.vl`, `AbstractConformance.vl`.

Non-régression à surveiller : `samples/Functional/FunctionalSpec.vl:50,66-67` (`.map` /
`.filter` point-free) est le seul consommateur existant de ces méthodes.

Puis mettre à jour `.agents/PLAN.md` : §VI items 2 et 5 → FAIT ; §VII.1 → FAIT, la prochaine
étape devient §VII.2 (codegen / `volt run`).

---

## 5. Pièges rencontrés (à ne pas re-découvrir)

- **`volt-build` ne recompile pas toujours** après une salve d'édits : `ninja: no work to do`
  alors que les sources ont changé. `touch` les `.cpp` concernés et relancer. Toujours vérifier
  qu'une ligne `Building CXX object …` apparaît avant de croire un résultat de `volt check`.
- **Le `Void` de Volt n'existe pas** comme type déclaré dans `source/Lib/` : `-> Void` résout
  vers un id invalide. Toléré partout (un id invalide veut dire « non inféré », jamais une
  erreur), mais c'est à garder en tête en lisant les types de `Proc`. L'ajouter serait un
  changement séparé, avec un risque de nouveaux diagnostics dans la stdlib.
- **Les diagnostics clang-tidy remontés par l'IDE** sur `TypeChecker.cpp`
  (`readability-redundant-member-init`, `modernize-use-designated-initializers`) sont
  **préexistants** et ne sont pas dans la configuration appliquée par `volt-build tidy`. Ne pas
  les traiter comme des régressions.
- **`volt check` charge toute la stdlib** : le compte de fichiers dans son résumé
  (« 15 file(s) ») inclut `source/Lib/`, pas seulement la cible.
