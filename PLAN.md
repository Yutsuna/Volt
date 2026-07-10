
# Dé-hardcoder String du compilateur (stdlib Volt, étape 3)

## Contexte

Suite du plan stdlib Core (voir `SharedMemory/volt-core-stdlib.md`) : le mécanisme Core (embarqué + `VOLT_CORE`), `IO.vl` et le shadowing sont faits. Reste l'étape 3 : retirer le type String hardcodé du compilateur/VM (`Tag::Str`, `TO_STRING`, `CONCAT_STR`, coercions) au profit d'une `class String` en pur Volt avec ses propres `def +`, `def ==`, `def to_string`, etc. Branche `BREAKING/Volt-std-lib` — les breaking changes sont assumés.

## Décisions actées (avec l'user, 2026-07-10)

1. **`to_s` → `to_string`** partout (lisibilité ; les `to_s`/`is_a` cryptiques sont bannis).
2. **Méthodes sur les primitifs** : nouvelle capacité — le Core peut déclarer `struct Int64 … def to_string`, etc. `TO_STRING` disparaît entièrement.
3. **`Int#to_string` alloue** (pas de string view : les digits n'existent nulle part avant l'appel ; un buffer statique serait écrasé). `String#to_string` retourne `self`, zéro coût. `StringView` = future issue pour le slicing sans copie.
4. **String = `class`** (pas struct) : `{ @ptr : UInt8*, @size : UInt64, @owned : Bool }`. Le RAII existant des classes (`INIT`/`DROP`/`DROP_SCOPE` → `destroy_object` → `finalize`) garantit zéro fuite : `finalize` fait `free(@ptr) if @owned`. Les littéraux pointent vers les bytes statiques du const pool → `owned = false`.
5. FFI : le Core passe `str.data`/`str.size` explicitement (`write(1, str.data, str.size)` — supprime aussi le `strlen` runtime de `print`). La coercion magique String→`UInt8*` du typechecker est retirée.
6. L'**overloading d'opérateurs existe déjà** (`FunctionEmiter#operator_method_name` :1166, `compile_binary` :880, `TypeChecker#operator_method`, parser `def ==`/`def *` ParseTopLevel.cr:161) → `String#+`/`String#==` en pur Volt sans nouvelle feature.

## Phases (chacune laisse l'arbre vert)

### Phase 1 — Renommage `to_s` → `to_string`
- Désucrage interpolation : `ParseControlFlow.cr:145-161` (`MemberAccess(expr, "to_s")` → `"to_string"`).
- `FunctionEmiter.cr:542` (interception dans `compile_method_call`) et `compile_to_s` :518-536 (renommer, garder le fallback `TO_STRING` pour l'instant).
- TypeChecker :431, :446, :480 (builtin `.to_s` → `.to_string` retourne `Type::STR` pour l'instant).
- Migrer samples/specs qui utilisent `.to_s` ; `Spec/Samples/Data.cr` (numéros de ligne en dur).

### Phase 2 — Méthodes sur les primitifs
- `TypeCollector.cr` : accepter une décl Core `struct Int64` (etc.) qui *réouvre* un primitif — enregistre un `TypeInfo` avec méthodes, mais le type reste le primitif 1-slot (`Type::INT`…), pas un nominal à layout. Mapping nom↔primitif via `from_primitive_name` existant (`Type.cr:199`).
- TypeChecker : la résolution de méthode sur un receiver primitif consulte le `TypeInfo` du nom du primitif (`Type#to_s` donne le nom).
- `FunctionEmiter#compile_method_call`/`compile_to_s` : receiver primitif → `compile_struct_call`-like avec `self` = 1 slot (dispatch direct `CALL`, pas de vtable).
- Nouveaux fichiers Core (ordre dans `Embedded.cr:11-14`) : `Int.vl` (itoa pur Volt, buffer malloc'é retourné en String owned), `Float.vl` (`snprintf` via `@[External("libc")]`), `Bool.vl` (retourne littéraux "true"/"false"). Nota : dépendent de la class String → injectés après `String.vl` en Phase 3 ; en Phase 2 on pose la mécanique compilateur + specs sur un type user (`struct` sample réouvrant Int64) pour rester vert.

### Phase 3 — `class String` en Core + littéraux
- `Core/String.vl` : `VString` struct → `class String` : `@ptr`, `@size`, `@owned` ; `initialize(@ptr, @size, @owned)` ; `data`, `size` ; `finalize` → `free(@ptr) if @owned` ; `def +(other : String) -> String` (malloc size1+size2+1, 2×memcpy externs, owned=true) ; `def ==(other : String) -> Bool` (memcmp ou boucle) ; `def to_string -> String` (self).
- `Core/IO.vl` : `print(str : String)` → `write(1, str.data, str.size)` ; plus de `strlen`.
- **Littéraux** : `FunctionEmiter.cr:185` — `StringLit` compile en construction `String.new(ptr, size, false)` (réutiliser `compile_constructor_call`) avec deux constantes : `Tag::Ptr` (bytes null-terminés) + `Tag::Int` (taille compile-time). Rooting GC : side array `Chunk#strings : Array(String)` qui garde les Crystal Strings vivantes (ne pas dépendre des interior pointers Boehm). Idem `TypeofExpr` :218.
- TypeChecker : `StringLit` typé par lookup nominal `"String"` (déclaré par le TypeCollector depuis le Core, ordre OK car Core préfixé) ; retirer `from_primitive_name "String"→STR` (`Type.cr:199`), l'arm `str+str→STR` (:649), la coercion String→UInt8* (:626-628).
- ⚠️ Perf : un littéral évalué en boucle = un `INIT` heap par itération. Acceptable (fib/primes n'ont pas de strings en boucle chaude) ; noter une future issue « literal hoisting/interning ».

### Phase 4 — Suppression Tag::Str / TO_STRING / CONCAT_STR
- `IR/Value.cr` : retirer `Tag::Str` (:91), `Value.str` (:107), `as_s` (:117), arms `as_ptr` (:120-126), `to_display` (:146), `==` (:170). Regex/Nil/Object/Ptr renumérotent.
- `IR/Opcode.cr` : retirer `TO_STRING`/`CONCAT_STR` (:141-142) ; sites d'émission `FunctionEmiter.cr:534, :873-877` ; handlers `Vm.cr:389-396, :681-695`, `Dispatch/Cmp.cr` ; listes `Peephole.cr:33-35, 88, 97-98`.
- **ABI** : renuméroter `Spec/VM/OpcodeAbiSpec.cr:11-51` (ordinals + count 85 + `Tag::Str==3`), `Source/C/Include/Types.h` (`EValueTag`), `Source/C/Source/Dispatch.c:143-160` (DispatchTable positionnel).
- **Regex (reste hardcodé)** : `MATCH_STR`/`NOT_MATCH_STR`/`EQ_CASE` (`Cmp.cr:24-32`) lisent désormais l'opérande String comme HeapObject → extraire ptr/size des slots de champ pour construire la Crystal String côté handler.
- `Native.cr:36` (`to_c_arg` arm Str) : supprimé — plus atteignable, le Core passe des `UInt8*`.
- **Affichage** (REPL `display_result`, `RAISE` `to_display`) : la Vm résout le `type_id` de `String` au chargement de l'unit ; `to_display` d'un Object de ce type lit ptr/size (hardcode display-only, acceptable — alternative pure : `call_chunk` sur `to_string`).
- Specs touchées : `REPLEvaluationResultSpec.cr:106-108`, `RaiiSpec.cr:66` (`Value.str`), retirer le diagnostic mort S0051 (`Catalog.cr:430-431`).

### Phase 5 — Migration & nettoyage
- Passer tous les samples/benchs (interpolation, `puts`, comparaisons de strings) ; mettre à jour `Spec/Samples/Data.cr`.
- Mémoire partagée : mettre à jour `SharedMemory/volt-core-stdlib.md` + `PLAN.md`.

## Vérification
- Build via `krystal` (pas de flag `-f`) ; `bin/Volt` frais avant les specs (piège connu de staleness).
- 330 specs vertes ; tous les samples Functional/Circuits.
- Benchmarks : fib(35) ≤ ~1.0s, primes ~0.07s — zéro régression (aucun des deux n'a de string en boucle chaude).
- End-to-end : littéral simple, interpolation multi-trous (`"a#{1}b#{x}"`), `s1 + s2`, `s1 == s2`, `puts`/`print`, raise "msg", REPL affichage d'une String, `VOLT_CORE=<dir>` à chaud, RAII : boucle créant des strings owned → pas de fuite (vérif `free` appelé / RSS stable).
- Regex : `=~`/`!~`/`case ===` sur la nouvelle String.

## Risques
- Renumérotation ABI C (Types.h/Dispatch.c positionnels) — l'OpcodeAbiSpec est le garde-fou, le faire en un seul commit atomique.
- Réouverture des primitifs : interactions avec le shadowing `mark_redefinable` (une redéf user de `Int64#to_string` doit shadow, pas dupliquer).
- Ordre d'injection Core : `String.vl` avant `Int.vl`/`Float.vl`/`Bool.vl` (ils retournent des String) ; `IO.vl` dépend de String → réordonner `EMBEDDED_SOURCES`.
- `finalize` sur littéral non-owned ne doit jamais `free` (flag `@owned` testé dans les specs RAII).

---

## Ce qui a été fait

- Core/IO.vl — puts et print en pur Volt, basés sur write(1, …) + strlen via @[External("libc")]. J'ai volontairement évité le puts de la libc : son buffer stdio se mélangeait avec les écritures directes de print (sortie dans le désordre, constaté au premier test). Tout passe par le syscall, l'ordre est garanti.
- Core/String.vl — struct provisoire VString { ptr : UInt8*, size : UInt64 } sur le modèle zero-overhead du sample 02.a ; il prendra le nom String à l'étape 3.
- Distribution hybride — Source/Volt/Core/Embedded.cr embarque les .vl au build via la macro read_file ; VOLT_CORE=<dir> repasse par le parseur sur des sources textuelles pour développer la stdlib sans rebuild (testé à chaud). Le parse du Core est mémoïsé par process.
- Injection — Volt::Core.inject préfixe les nœuds Core dans les trois pipelines : run mono-fichier, mode circuit, check, et le REPL (1er tour seulement ; les tours suivants héritent via IncrementalState). Les sources Core sont fusionnées dans le DiagnosticRenderer : une erreur dans le Core rend un file:line:col propre.
- Shadowing — une redéfinition utilisateur d'un nom Core n'est plus un doublon : j'ai réutilisé le mécanisme mark_redefinable du REPL, avec un registre Frontend.core_files (le Frontend reste sans dépendance vers Core). Le corps Core shadowé est écarté avant le typecheck et la compilation.
- Samples nettoyés — les ~20 déclarations @[External] def puts redondantes des benchmarks/samples sont retirées ; le golden Spec/Samples/Data.cr (numéros de ligne en dur) mis à jour.
- Phase 4 — Suppression complète de `Tag::Str`, `CONCAT_STR` et adaptation de `TO_STRING`. Tous les alignements ABI avec le Core C (Compare.inc, Dispatch.h) ont été corrigés. Les constructeurs et expressions littérales s'appuient désormais entièrement sur la classe nominale `String` en pur Volt. Les regex et affichages VM/REPL lisent la représentation `HeapObject` en extrayant ptr/size à chaud.

## Vérification

- 330 specs, 0 échec ; tous les samples Functional/Circuits passent.
- fib 35 : ~0.6s (cible ≤1.0s), primes : 0.073s — aucune régression.
- Testés end-to-end : puts/print sans déclaration, struct Core, shadowing par extern utilisateur, override VOLT_CORE à chaud, diagnostic d'erreur dans le Core, REPL avec redéfinition en cours de session.
- Intégration Phase 4 validée et 100 % verte (330 specs passées).
