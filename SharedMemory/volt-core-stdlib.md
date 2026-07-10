---
name: volt-core-stdlib
description: "Volt Core stdlib — architecture décidée (hybride embarqué + VOLT_CORE, 100% pur Volt sans @[Primitive]) et état d'avancement"
metadata: 
  node_type: memory
  type: project
---

Décisions actées (2026-07-09) pour la stdlib Volt ("Core", auto-linkée, ni battery/circuit/link/external côté user) :
- **Distribution hybride** : `Core/*.vl` embarqués dans bin/Volt via macro Crystal `read_file` (`Source/Volt/Core/Embedded.cr`) + `VOLT_CORE=<dir>` pour dev à chaud sans rebuild. Futur (IR stabilisé) : embarquer du pré-compilé binaire, VOLT_CORE textuel reste le mode dev.
- **Pas de `@[Primitive]`/intrinsics VM** — choix explicite de l'user : un futur compilateur natif Volt rendrait des intrinsics VM impertinents. Stdlib = pur Volt + `@[External("libc")]`. Le coût des CALL sera résorbé par l'**aggressive inlining** (`@[Inline]` + heuristique avant Peephole) = prochaine issue à ouvrir.
- **Shadowing** : une redéf user d'un nom Core n'est pas un doublon — réutilise `SignatureTable#mark_redefinable` (mécanisme REPL) ; `Frontend.core_files` (Set peuplé par `Volt::Core`) marque la provenance ; les analysers filtrent les corps Core shadowés (`@functions.select!` sur `decl_span`).
- Fait : mécanisme Core + IO.vl (puts/print pur Volt via write/strlen syscall — pas de stdio libc pour éviter le désordre de buffers) + String.vl (struct `VString` provisoire ptr/size). Injection dans Run/Check/REPL (REPL: 1er tour seulement, ensuite hérité via IncrementalState).
- Fait (Étape 3 / Phase 4) : String dé-hardcodé. Littéraux compilés en appel constructeur nominal `String.new(ptr, size, false)`. `Tag::Str` et `CONCAT_STR` supprimés du compilateur, de l'ABI FFI (C Core) et de la VM. `TO_STRING` conservé uniquement pour le fallback Float. Regex lit le `HeapObject` nominal String à chaud pour extraire ptr/size. Hash/Array bloqués par les generics sémantiques (parsés mais non supportés).
- Piège : la forme `def puts(...) -> Int32` + `end` orphelin existe dans certains samples ; `Spec/Samples/Data.cr` contient des numéros de ligne en dur.

Voir [[volt-perf-milestone]] (fib ~0.6-0.7s, primes ~0.07s inchangés après Core) et [[build-env-krystal-crystal]].
