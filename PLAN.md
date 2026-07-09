## Ce qui a été fait

- Core/IO.vl — puts et print en pur Volt, basés sur write(1, …) + strlen via @[External("libc")]. J'ai volontairement évité le puts de la libc : son buffer stdio se mélangeait avec les écritures directes de print (sortie dans le désordre, constaté au premier test). Tout passe par le syscall, l'ordre est garanti.
- Core/String.vl — struct provisoire VString { ptr : UInt8*, size : UInt64 } sur le modèle zero-overhead du sample 02.a ; il prendra le nom String à l'étape 3.
- Distribution hybride — Source/Volt/Core/Embedded.cr embarque les .vl au build via la macro read_file ; VOLT_CORE=<dir> repasse par le parseur sur des sources textuelles pour développer la stdlib sans rebuild (testé à chaud). Le parse du Core est mémoïsé par process.
- Injection — Volt::Core.inject préfixe les nœuds Core dans les trois pipelines : run mono-fichier, mode circuit, check, et le REPL (1er tour seulement ; les tours suivants héritent via IncrementalState). Les sources Core sont fusionnées dans le DiagnosticRenderer : une erreur dans le Core rend un file:line:col propre.
- Shadowing — une redéfinition utilisateur d'un nom Core n'est plus un doublon : j'ai réutilisé le mécanisme mark_redefinable du REPL, avec un registre Frontend.core_files (le Frontend reste sans dépendance vers Core). Le corps Core shadowé est écarté avant le typecheck et la compilation.
- Samples nettoyés — les ~20 déclarations @[External] def puts redondantes des benchmarks/samples sont retirées ; le golden Spec/Samples/Data.cr (numéros de ligne en dur) mis à jour.

## Vérification

- 330 specs, 0 échec ; tous les samples Functional/Circuits passent.
- fib 35 : ~0.6s (cible ≤1.0s), primes : 0.073s — aucune régression.
- Testés end-to-end : puts/print sans déclaration, struct Core, shadowing par extern utilisateur, override VOLT_CORE à chaud, diagnostic d'erreur dans le Core, REPL avec redéfinition en cours de session.

Reste (tâche #5)
