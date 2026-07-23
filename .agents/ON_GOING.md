# ON_GOING — Mixins & méthodes d'ordre supérieur (dernière étape du MiddleEnd)

**Branche :** `Feat/Finish-Frontend`
**Dernier point de contrôle :** phases 0 à 4 **terminées et validées**. Phase 5 **en cours** (code écrit, build vert, mais elle révèle 66 non-conformités réelles dans `source/Lib/` — voir §4). Phase 6 non commencée.
**État build/tests au dernier point vert (fin phase 4) :** `volt-build test` **98/98**, `volt check source/Lib/` **0 erreur**.

Ce document est autonome : il suffit à reprendre le chantier dans une nouvelle session sans relire l'historique de conversation. Le plan d'origine est dans `~/.claude/plans/sprightly-orbiting-clock.md`, mais **ce fichier fait foi** — il intègre les écarts découverts pendant l'implémentation.

> **Convention de travail (importante).** Ne pas `git commit` de sa propre initiative. Le chantier se laisse dans le working tree ; c'est l'utilisateur qui découpe et rédige ses commits.

---

## 1. Le problème, en une page

L'objectif est celui de `PLAN.md §VII.1` : que `Array.vl` reçoive `.map()` / `.filter()` du mixin `Enumerable`. L'injection des membres de mixin *fonctionnait déjà* (`TypeStore::LookupMember` traverse corps propre → `Includes` → `Super`). Ce qui manquait est en amont et en aval, en **quatre trous silencieux** :

| # | Trou | État |
|---|---|---|
| 1 | le type `self` d'une signature ne résolvait pas → tout `Comparable`/`Arithmetic` renvoyait un type invalide, ce qui désarmait le diagnostic de membre inconnu | **CORRIGÉ (phase 1)** |
| 2 | aucun type stdlib ne réclamait `FuncType`/`Lambda`/`Block` → lambdas et blocs non typés | **CORRIGÉ (phase 0)** |
| 3 | les arguments génériques d'`include` sont jetés (`Includes` est un `SmallVec<NominalId>`) et `LookupOn` n'applique les args du receveur que si `Owner == Base` | **CORRIGÉ (phase 2)** |
| 4 | les paramètres d'un `do \| x \|` n'héritent pas du `&block : T -> Void` déclaré | **CORRIGÉ (phase 3)** |

Plus un manque de langage : `Frontend::Method` n'avait pas de `Generics`, donc `def map<U>( &block : T -> U ) -> Array<U>` était inexprimable — **CORRIGÉ (phase 4)**.

### Sondes de référence

Fichier jetable + `volt check <fichier>`. C'est le moyen le plus rapide de mesurer l'avancement. **Lancer depuis la racine du dépôt** (voir §6, la découverte de la stdlib est relative au CWD).

```volt
def probe -> Void
  a = 3
  x = a.min( "oops" )              # phase 1 ✓ argument 1 to min has type String, expected Int32
  y = a.abs.nonexistent_method     # phase 1 ✓ type Int32 has no member 'nonexistent_method'
  arr = [ 1, 2, 3 ]
  arr.each do | i |
    i.no_such                      # phase 3 ✓ type Int32 has no member 'no_such'
  end
  arr.map do | i | i > 1 end       # phase 6 → doit valoir Array<Bool> (machinerie prête, stdlib à écrire)
end
```

---

## 2. Ce qui est fait

Les phases 0 et 1 sont décrites dans l'historique git (commits jusqu'à `ddfe2fa`). Ce qui suit couvre les phases 2 à 4, réalisées dans cette session ; l'utilisateur en a commité l'essentiel au fil de l'eau (`191f2c3` → `63d55dd`), et le working tree porte le reste.

### Phase 2 — `include Mixin<Args>` et lookup instancié ✅

**Stockage.** `NominalType::Super` et `NominalType::Includes` passent de `NominalId` à **`SigTypeId`** (`TypeStore.hpp`) : l'`include` est gardé *tel qu'écrit*, dans l'espace de paramètres de l'incluant. `include Enumerable<T>` sur `Array<T>` ⇒ `SigType{ Base = Enumerable, Args = [ ParamIndex 0 ] }`.
- `SetSuper` / `AddInclude` changent de signature.
- `TypeBinder.cpp` : `NominalOf` (qui ne gardait que la base) est remplacé par **`ParentOf`**, qui renvoie le `ResolveTypeExpr` complet. Utilisé pour `Super` *et* `Include`.

**Deux nouveaux helpers sur `TypeStore`** (`TypeStore.hpp`), pour que les deux traversées ne puissent pas diverger :
- `OwnMember( NominalId, Name ) -> const Member *` — le corps propre uniquement. `LookupMember` est réécrit par-dessus.
- `BaseOf( SigTypeId ) -> NominalId` — la base d'un lien parent, en ignorant ses arguments.

**Lookup instancié.** Nouvelle fonction libre dans `TypeResolve.hpp/.cpp` (là où `UnitTypes` est disponible pour interner) :

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

Corps propre → chaque `Includes[i]` instancié contre les args du receveur → `Super`, même ordre que `LookupMember`, profondeur bornée à 16. **`Self` reste toujours le receveur d'origine**, jamais le mixin traversé — c'est ce qui rend `Comparable#<( other : self )` correct sur `Int32`.

> ⚠️ **Piège d'invalidation mémoire.** `Instantiate` interne dans `Values` et peut déplacer l'arène. `LookupMemberOn` **copie** `Values.Get( Receiver )` dans un `SemaType` local avant d'instancier ; `LookupOn` copie de même `Values.Get( Found.Owner ).Args`. Ne pas « optimiser » ces copies en références.

**`LookupOn`** (`MemberResolver.cpp`) : la garde `if ( Found.Owner == Base )` disparaît ; `Applied` devient les args de `Found.Owner`.

**Correctif attrapé au passage — `T.new`.** `new` retombait sur `initialize`, dont le résultat déclaré est le `Void` que tout initialiseur écrit ; `Holder.new` sortait donc non typé. `LookupOn` marque `bConstructor` et substitue le receveur comme résultat. **La phase 6 en dépend** (`Array<U>.new`).

**Constantes** (`MemberResolver.hpp`) : `ConstructorCall = "new"` / `ConstructorName = "initialize"`. Ce sont de la syntaxe Volt, pas des noms de type — ZeroHardcode n'est pas concerné.

**Validation** (rejouée, OK) :
```volt
mixin Box<T>
  abstract def get -> T
  def get_twice -> T
    get
  end
end
struct Holder
  include Box<Int32>
  def get -> Int32
    0
  end
  def probe -> Void
    self.get_twice.no_such_either   # ✓ type Int32 has no member 'no_such_either'
  end
end
```

### Phase 3 — typage descendant des blocs ✅

1. `Resolution` (`TypeCheckerContext.hpp`) gagne **`SemaTypeId BlockParam`** — le slot `&block` instancié. La boucle sur `ParamIsBlock` le *capture* au lieu de le jeter.
2. `TypeCheckerContext` gagne **`SemaTypeId ExpectedClosure`**.
3. **`CallType` réordonné** (`ExprInferencer.cpp`) : callee d'abord (c'est lui qui remplit `CalleeResolution`), puis les arguments, puis le `BlockArg` sous `ExpectedClosure = Found.BlockParam`, avec sauvegarde/restauration.
4. `BindClosureParams` (`ClosureInferencer.cpp`) consomme `ExpectedClosure` : un paramètre sans annotation prend `Expected.Args[ 1 + Index ]` (rappel : `Args[0]` est le résultat). **Il remet `ExpectedClosure` à invalide immédiatement après lecture**, pour qu'une closure imbriquée plus profond ne le récupère pas par accident.

**Validation** (OK) : `arr.each do | i | i.no_such end` → *type Int32 has no member 'no_such'*.

### Phase 4 — génériques au niveau méthode (`def map<U>`) ✅

**Frontend :**
- `Frontend::Method` (`AST/Decl.hpp`) gagne `SymbolList Generics;` après `Name`.
- `ParseDecl.cpp`, `ParseMethod` : `Node.Generics = ParseGenericParams();` **après** la consommation du nom et **avant** le `LParen`. Sans danger pour `def <( … )` / `def <=>` : le nom-opérateur est déjà consommé quand l'helper regarde le `<`.
- **Les goldens existants n'ont PAS bougé** : le dumper réflexif n'émet pas une `SymbolList` vide. Un `def map<U>` s'affiche `Method 'map' [U]`. Aucune regénération n'a été nécessaire.

**Sema — espace de paramètres concaténé :**
- `SignatureResolver` (`TypeBinder.cpp`) construit un span `Space = [ génériques du type…, génériques de la méthode… ]` et résout toute la signature contre lui. Sur `Array<T>`, le `U` de `def map<U>` est donc `ParamIndex 1`, et `Instantiate` n'a besoin d'aucun concept supplémentaire.
- `Member` gagne `std::uint32_t OwnGenerics` (la coupure) et le binder le remplit.
- **`UnifySig`** (`TypeResolve.hpp/.cpp`) : parcours parallèle motif/type concret. Un `ParamIndex` **non encore lié** se lie ; un slot déjà lié gagne (donc un mauvais argument reste un diagnostic au lieu de redéfinir `T` en silence) ; `self` n'apprend rien ; une base nominale différente arrête la descente. Aucun diagnostic propre, conformément à la discipline du fichier.

**Le flux d'inférence**, réparti entre `MemberResolver.cpp` et `CallType` :
- `Resolution` gagne `Core::SmallVec<SemaTypeId,2> Bindings` (args du propriétaire ++ un trou par générique de méthode) et `SemaTypeId Receiver` (pour re-résoudre `self` à l'identique).
- **`Reinstantiate( Context, Resolution& )`** — recalcule `Result` / `Params` / `BlockParam` depuis `Bindings`. Idempotent, rappelé à chaque trou refermé.
- **`UnifyArgs`** — lie depuis les arguments positionnels (en sautant le slot `&block` **exactement comme `Reinstantiate`**, pour que l'argument N désigne le même paramètre des deux côtés), puis recalcule.
- **`UnifyBlock`** — lie depuis le type réel du bloc, puis recalcule. C'est là que `U` est appris.
- `CallType` orchestre : callee → args → `UnifyArgs` → `CheckCallArgs` → bloc sous `ExpectedClosure` → `UnifyBlock` → **retourne `Found.Result` et non la valeur d'origine du callee** (qui avait encore ses trous ouverts).

**Validation** (OK) — `Mapper<T>` jouet avec `def convert<U>( &block : T -> U ) -> Array<U>` :
`self.convert do | i | i > 1 end` → `Array<Bool>`, et `.pop` dessus → `Bool`. La chaîne complète marche : `T` vient de l'`include`, `i` est typé `Int32` par la phase 3, le bloc rend `Bool`, `U` s'unifie, `Array<U>` se ré-instancie.

---

## 3. Ce qu'il reste à faire

### Phase 5 — conformité des `abstract def`  ← **EN COURS, ÉTAT PRÉCIS CI-DESSOUS**

**Ce qui est déjà écrit et compile** (dans le working tree) :
- `Member::bAbstract` (`TypeStore.hpp`), rempli par `DeclareMembers` (`TypeBinder.cpp`), à côté de `bApply`.
- `DeclStmtWalker.cpp`, namespace anonyme en tête de fichier :
  - `CollectIncludes( Store, Id, Out, Depth )` — tous les mixins atteignables par `include`, transitivement, sans doublon. **Le `Super` n'est délibérément pas suivi** : ce qu'il inclut, il a lui-même été tenu de l'implémenter.
  - `CheckAbstractConformance( Context, Id, Loc )` — pour chaque membre `bAbstract` d'un mixin inclus, re-résoudre le nom depuis `Context.SelfValue` via `LookupMemberOn` et exiger de tomber sur une déclaration **non abstraite**. Une implémentation héritée d'une superclasse ou fournie par un autre mixin compte donc.
- `EnterType` gagne deux paramètres : `Core::SourceRange Loc` et `bool bConcrete`. `WalkDecl` passe `true` pour `Struct`/`Class`, **`false` pour `Mixin`** — un mixin a le droit de faire suivre un contrat. `Proc#call` n'est pas signalé non plus, puisque le contrôle ne porte que sur les mixins *inclus* et que `Proc` déclare son `call` lui-même.

**Ce qui bloque : la stdlib n'honore pas ses propres contrats — 66 erreurs.**

`mixin Arithmetic` déclare `abstract def + - * / % & | ^ << >> ~`, et **aucun** `Int8/Int16/Int32/Int64/UInt*/Float*` ne les implémente. Ce n'est pas un bug du contrôle : sur un primitif, `+` est une instruction machine, pas une méthode Volt. Le compilateur connaît déjà cette échappatoire — `MemberType` (`MemberResolver.cpp`) supprime le diagnostic « has no member » quand le receveur a un layout `Primitive`/`Pointer` **et** que le nom passe `IsBuiltinPrimitiveOp`.

**Le correctif décidé, à appliquer :** factoriser ce prédicat pour que **les deux sites l'honorent identiquement**, au lieu de le dupliquer.

```cpp
// MemberResolver.hpp — à ajouter
// True quand `Name` est un opérateur que le backend fournit directement sur un
// type de layout primitif ou pointeur. Volt déclare ces membres comme contrats
// abstraits (`mixin Arithmetic`) mais n'en écrit jamais le corps. Le diagnostic
// de membre inconnu ET le contrôle de conformité doivent honorer la même
// exemption, sinon l'un contredit l'autre.
[[nodiscard]] bool IsBuiltinOpOn ( const TypeCheckerContext &Context, NominalId Base, std::string_view Name );
```

Extraire le corps depuis `MemberType` (`MemberResolver.cpp`, le bloc `Nominal.Layout.IsValid()` → `KindOf(...) == Primitive|Pointer` + `IsBuiltinPrimitiveOp( Name )`), faire appeler ce nouveau prédicat par `MemberType`, puis ajouter dans la boucle de `CheckAbstractConformance` :

```cpp
if ( IsBuiltinOpOn( Context, Id, Name ) )
{
    continue;
}
```

Attendu après ce correctif : `volt check source/Lib/` **revient à 0 erreur**, et un `struct` non primitif qui inclut un mixin sans implémenter son `abstract def` est bien signalé.

**Second point à finir : la localisation du diagnostic.** `Node.Loc` d'un `Struct` couvre **toute la déclaration**, donc le caret souligne 200 caractères et le message est illisible :

```
source/Lib/Primitives/Int.vl:71:1: error: type Int64 does not implement abstract member '<<' ...
    struct Int64
    ^~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ (…)
```

`Core::SourceRange` est `{ FileId File; std::uint32_t Begin; std::uint32_t End; }` (`Core/Diagnostics/SourceLocation.hpp`). La correction minimale est de resserrer sur le début : passer `Core::SourceRange{ .File = Node.Loc.File, .Begin = Node.Loc.Begin, .End = Node.Loc.Begin }` — ou, mieux, chercher s'il existe déjà un helper de troncature dans `SourceLocation.hpp` (il y a un `Merge`, vérifier s'il y a un équivalent « point de départ »). **Ce point n'a pas été tranché.**

**Sonde de validation de la phase 5** (à rejouer une fois le prédicat factorisé) :
```volt
mixin Sized
  abstract def size -> UInt64
end
struct Good
  include Sized
  def size -> UInt64
    0_u64
  end
end
struct Bad
  include Sized      # doit être la SEULE erreur du fichier
end
```

---

### Phase 6 — stdlib `Enumerable<T>`

**La machinerie est prête** (phases 2 à 4 validées sur des mixins jouets équivalents). Il reste à écrire le Volt.

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

Les autres mixins (`Comparable`, `Arithmetic`, `Hashable`) ne changent pas de forme — leur `self` est simplement *résolu* depuis la phase 1.

**Risque déjà sondé et levé :** `Array<U>.new` puis `.push` dans le corps de `map` fonctionne — c'est exactement ce que fait `Mapper<T>#convert` dans la sonde de la phase 4, et le correctif `bConstructor` de la phase 2 était le maillon manquant.

**Risque restant, non levé — les appels à receveur implicite.** Dans le corps d'un mixin, `each do | item | … end` (sans receveur) se parse en `Call{ Callee = Identifier 'each' }`. La branche `Identifier` de `ComputeExpr` ne cherche qu'un local puis un nom de type : **elle ne résout pas un membre de `self`**, donc `CalleeResolution` reste vide, et ni le typage descendant du bloc ni l'inférence des génériques ne s'appliquent. `self.each do … end` marche, `each do … end` non.

C'est un trou **préexistant**, indépendant de ce chantier, mais il touche directement l'écriture des corps de `map`/`filter`/`any?`. Deux options :
1. écrire les corps avec `self.` explicite (contournement immédiat, un peu inélégant en Volt) ;
2. corriger la branche `Identifier` pour retomber sur `LookupOn( Context, Context.SelfValue, Name )` quand le nom n'est ni un local ni un type — ce qui est probablement la bonne correction de fond, et rendrait aussi `DotCall` et `Identifier` cohérents. **À trancher avant d'écrire la phase 6.**

---

## 4. Clôture

```sh
volt-build format test          # -Werror ; 98/98 à la fin de la phase 4
volt check source/Lib/          # doit revenir à 0 erreur après le correctif §3 phase 5
graphify update .               # TypeResolve (LookupMemberOn, UnifySig), Method::Generics
```

Nouveaux goldens à créer sous `tests/golden/samples/Sema/` (format des `UnknownMember.vl.golden` existants : un `.golden` + un `.lowered.golden`) : `MixinGenerics.vl`, `BlockParamTypes.vl`, `AbstractConformance.vl`. **Aucun n'est encore écrit.**

Non-régression à surveiller : `samples/Functional/FunctionalSpec.vl:50,66-67` (`.map` / `.filter` point-free) est le seul consommateur existant de ces méthodes.

Puis mettre à jour `.agents/PLAN.md` : §VI items 2 et 5 → FAIT ; §VII.1 → FAIT, la prochaine étape devient §VII.2 (codegen / `volt run`).

---

## 5. Sondes jetables de la session

Elles sont dans `$CLAUDE_JOB_DIR/tmp` (éphémère). À recréer au besoin ; les sources exactes sont reproduites en §2 (phases 2 et 4) et §3 (phase 5).

---

## 6. Pièges rencontrés (à ne pas re-découvrir)

- **`volt-build` ne recompile pas toujours** après une salve d'édits : `ninja: no work to do` alors que les sources ont changé — et le symptôme peut être un **échec de link fantôme** (`FAILED: lib/libSema_d.so`) suivi d'un `no work to do` au run suivant. `touch source/Volt/Sema/Private/Passes/TypeChecker/*.cpp source/Volt/Sema/Private/Layout/*.cpp` puis relancer. Toujours vérifier qu'une ligne `Building CXX object …` apparaît avant de croire un résultat de `volt check`.
- **`volt check` découvre la stdlib relativement au CWD.** Lancé depuis un autre répertoire, il ne charge que le fichier cible (« 1 file(s) ») et sort des erreurs absurdes du genre *no type claims IntLiteral*. **Toujours lancer depuis la racine du dépôt**, même avec un chemin absolu en argument. Le compte de fichiers du résumé (« 15 file(s) ») inclut `source/Lib/`, pas seulement la cible.
- **Le `Void` de Volt n'existe pas** comme type déclaré dans `source/Lib/` : `-> Void` résout vers un id invalide. Toléré partout (un id invalide veut dire « non inféré », jamais une erreur), mais c'est à garder en tête en lisant les types de `Proc`. L'ajouter serait un changement séparé, avec un risque de nouveaux diagnostics dans la stdlib.
- **Invalidation d'arène.** `Instantiate` / `ResolveTypeExpr` internent dans `UnitTypes` et peuvent déplacer l'arène : ne jamais garder une `const SemaType &` (ni une référence sur ses `Args`) à travers un de ces appels. `LookupMemberOn`, `LookupOn` et `BindClosureParams` copient explicitement pour cette raison.
- **Les appels à receveur implicite ne résolvent pas** (`foo( x )` / `foo do … end` sans `self.`) — voir §3 phase 6. Piège de sonde : un test qui « passe » silencieusement alors qu'on attendait une erreur vient souvent de là. Vérifier avec `self.` avant de conclure que la machinerie est cassée.
- **Les diagnostics clang-tidy remontés par l'IDE** sur la pass `TypeChecker` (`readability-redundant-member-init`, `modernize-use-designated-initializers`, `misc-use-internal-linkage`, `modernize-use-ranges`) sont **préexistants ou hors configuration** appliquée par `volt-build tidy`. Ne pas les traiter comme des régressions.
- **`-Wmissing-designated-field-initializers` est fatal** (`-Werror`). Toute construction de `Resolution{ .Decl = …, … }` doit lister **tous** les champs — d'où les `.Bindings = {}` / `.BlockParam = SemaTypeId{}` explicites dans `LookupOn`.
- **ZeroHardcode (`tests/ZeroHardcode.cmake`) scanne aussi les identifiants C++**, pas seulement les chaînes : la liste inclut `Char`, `Int`, `Bool`, `Hash`, `Nil`… Une variable locale nommée `Char` fait échouer le test 98. Corrigé en `Ch` (commit `191f2c3`).
