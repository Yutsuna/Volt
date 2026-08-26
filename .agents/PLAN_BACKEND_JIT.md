# Plan : BackendJIT — Compilation à la Volée pour Volt

*2026-08-20 — issue #120. Périmètre : `source/Volt/Backend/**`, `source/Volt/Driver/**`, `source/Volt/Volt/Private/Volt/CLI/**`, `.agents/**`.*

---

## 1. Vision & Principes

### 1.1 Ce qu'on construit

| Commande | Backend | Comportement |
|---|---|---|
| `volt build` | `BackendLLVM` | AOT natif, production — **inchangé** |
| `volt run` | **`BackendJIT`** | JIT, boucle de développement |
| `volt run --watch` | **`BackendJIT`** | JIT + rechargement à chaud |
| `volt repl` | **`BackendJIT`** | JIT incrémental |
| `volt build-stdlib` | `BackendLLVM` | pré-compilation de la stdlib |

Objectif chiffré, repris de `backend/vm.md` : **sous 100 ms** entre `volt run` et la première sortie, sur un
fichier utilisateur modeste, stdlib chaude.

### 1.2 D'où vient le temps

`volt run` n'existe pas ; le proxy est `volt build`. Deux caches amortissent déjà une partie du coût :

- **cache frontend** (`Driver.cpp:694-756`, magie `VOLTFE14`) — sérialise AST + sema de la stdlib ;
- **cache d'artefact natif** (`DriverBuild.cpp:55-111`) — produit un `.a` ou un `.so` de la stdlib.

Ce qui reste payé à chaque invocation : construire un `llvm::Module` monolithique, le vérifier, l'optimiser,
émettre un `.o`, appeler `cc`/`mold`. **C'est exactement ce que le JIT supprime.** Il ne supprime ni le parse
ni la sema du code utilisateur — déjà parallélisés par unité, et légitimes.

### 1.3 La recommandation contraire

`AUDIT.md` §13 (même date) conclut : *« Conserver LLVM comme backend principal et ne pas ajouter de JIT natif
pour l'instant »*. Ce plan est produit en connaissance de cause. Ce qui la renverse :

1. **L'audit évaluait une VM bytecode** — un second générateur complet (émetteur + ISA + boucle
   d'interprétation + désassembleur). Un JIT ORC n'est pas ça : le générateur existe déjà, on rebranche sa
   sortie. Le coût réel est une **extraction de module**, pas une réimplémentation.
2. **L'audit chiffrait 60 % de « glue Volt » réutilisable** sans dire vers quoi. Ce plan le dit :
   `BackendLlvmIr`, partagé AOT + JIT, duplication nulle.
3. Les deux *gaps bloquants pour un MVP VM* de sa §12 (`self` inaccessible en closure, `break <value>` refusé)
   **ne bloquent pas un JIT** : il exécute l'IR que l'AOT exécute, donc hérite des mêmes limitations, ni plus
   ni moins. Ce qui compile avec `volt build` tourne avec `volt run`.

L'audit reste juste sur un point qu'il faut acter : **le poids de maintenance augmente**. La mitigation est
structurelle (§2) — un seul émetteur sémantique, deux queues courtes et disjointes.

### 1.4 Principes non négociables

1. **`BackendJIT` ne dépend jamais de `BackendLLVM`** — `grep -r BackendLLVM source/Volt/Backend/BackendJIT/` → 0.
2. **`BackendLLVM` reste intact fonctionnellement** — `volt build` produit le même binaire avant/après.
3. **Zéro duplication** — tout ce qui est commun est extrait, jamais copié.
4. **Zéro header LLVM dans `Public/` de `BackendJIT`** — pimpl, comme `LlvmEmitter.hpp` le fait déjà.
5. **La stdlib n'est jamais compilée par le JIT** — elle est chargée pré-compilée.
6. **Un seul émetteur sémantique** — `BodyEmitter` et ses satellites existent en un exemplaire.
7. **`rules/backend-machine-only.md` s'applique intégralement** — le JIT lit `UnitTypes` / `UnitCallees` /
   `MemoryLayout` et émet. Deux modes d'échec, `Unimplemented` et `Error`, jamais un diagnostic utilisateur.

---

## 2. Architecture

### 2.1 Pourquoi la table de migration du brief ne peut pas s'appliquer

Le brief demandait de migrer `BodyEmitter`, `ExprEmitter`, `StmtEmitter`, `ExceptionLowering`,
`ClosureLowering`, `MonoDriver`, `FunctionRegistry`, `SignatureBuilder`, `ParameterBinder` vers `BackendCore`,
règle : *« aucun `#include <llvm/...>` et aucun type `llvm::` → candidat »*.

Mesures réelles sur les **82 fichiers / 8 403 LOC** de `BackendLLVM/Private` :

| Catégorie | Fichiers | LOC | Contenu |
|---|---|---|---|
| **PURE-LLVM** | 14 | ~1 100 | `Target/**`, `ModuleContext`, `EntryPointEmitter`, `SymbolNameEmitter`, `ExceptionGlobals`, `ClosurePairType`, `LlvmFwd` |
| **LLVM-FREE** | 11 | ~800 | `DiagnosticSink`, `LlvmBackend.cpp`, `ExprOperatorEmitter`, `Mono/{MonoDriver.*,MonoLookup}`, `StmtEmitter.hpp`, `TargetPipeline.hpp`, `LinkerDriver.hpp`, `LayoutOfValue` |
| **MIXED** | 57 | ~6 500 | l'émission proprement dite |

Les 57 MIXED sont le cœur : ils construisent des `llvm::Value*` via `llvm::IRBuilder<>` — **139 appels
`Create*` sur 34 fichiers**, 47 références à `ModuleContext::Builder()`. `llvm::Value*`, `llvm::Type*`,
`llvm::BasicBlock*` sont la monnaie de **toutes** les signatures de `BodyEmitter.hpp`, `ExprEmitter.hpp`,
`ExceptionLowering.hpp`, `ClosureLowering.hpp`, `FunctionFrame.hpp`.

Deux issues, toutes deux mauvaises :

- **faire entrer LLVM dans `BackendCore`** — mais `BackendWASM` linke `backendcore_dep`, et c'est précisément
  le backend « zéro dépendance externe ». On lui imposerait `libLLVM.so` pour rien ;
- **abstraire derrière `IIrBuilder` et implémenter deux fois** — mais les deux consommateurs émettent *le
  même* IR. On écrirait deux fois le même builder : principe #3 violé.

Il n'y a pas de troisième issue tant que les deux consommateurs sont des backends LLVM. **Le partage ne se
fait pas dans `BackendCore`, il se fait un cran au-dessus.**

### 2.2 Trois couches

```
                    ┌─────────────────────────────────────────┐
                    │  MiddleEnd — core AST, 25 nœuds, typé   │
                    └────────────────────┬────────────────────┘
                                         │ BackendInput / UnitView
                    ┌────────────────────▼────────────────────┐
                    │  BackendCore            (zéro LLVM)     │
                    │  LayoutEngine · Mangler · SymbolRegistry│
                    │  InstanceLayouts · Monomorphizer        │
                    │  VTableEngine · UnwindTransport         │
                    │  ClosureABI · InitAllSynthesizer        │
                    │  AbiClassifier* · Instructions.inl*     │
                    │  TargetBackend · IJitBackend*           │
                    │  DiagnosticSink*                        │
                    └───┬──────────────────────────────────┬──┘
                        │                                  │
         ┌──────────────▼──────────────┐      ┌────────────▼────────────┐
         │  BackendLlvmIr*  (LLVM Core)│      │  BackendWASM            │
         │  ─────────────────────────  │      │  (encodeur autonome)    │
         │  AST  ->  llvm::Module      │      └─────────────────────────┘
         │  ModuleContext · TypeMapper │
         │  Functions/** · Lower/**    │
         │  ~55 fichiers, ~6 900 LOC   │
         └───┬─────────────────────┬───┘
             │                     │
  ┌──────────▼─────────┐  ┌────────▼───────────┐
  │  BackendLLVM       │  │  BackendJIT*       │
  │  (queue AOT)       │  │  (queue ORC)       │
  │  Optimizer         │  │  JitCompiler       │
  │  ObjectEmitter     │  │  JitSymbolResolver │
  │  LinkerDriver      │  │  JitRuntime        │
  │  StdlibArtifact    │  │  JitStdlibLoader   │
  │  ~15 fichiers      │  │  JitEntryPoint     │
  └────────┬───────────┘  └────────┬───────────┘
           │                       │
      volt build              volt run / repl
```

`*` = nouveau ou déplacé par ce plan.

### 2.3 Pourquoi cette forme

- `BackendJIT` ne voit jamais `BackendLLVM` — contrainte #1 structurelle, pas conventionnelle.
- L'IR est produit une fois par du code écrit une fois. Un bug d'émission se corrige à un seul endroit.
- Les deux queues sont **courtes et disjointes** : l'AOT sait sérialiser et lier, le JIT sait mapper et
  appeler. Elles ne partagent rien parce qu'elles n'ont rien à partager.
- `BackendCore` retrouve sa définition affichée — *« la couche que les trois cibles consomment »* — sans LLVM
  dedans. `BackendWASM` ne régresse pas.
- Le concept `TargetBackend` est respecté par les trois backends **sans modification**.

### 2.4 Le sort de `BackendVM`

Le squelette (5 fichiers, ISA bytecode complète dans `Bytecode.inl`, tout le reste `Unimplemented`) est
**compilé et installé mais lié à rien** : `backendvm_dep` n'a aucun consommateur dans le dépôt, et son
`subdir()` est *après* `subdir('Driver')`, donc la variable n'existe même pas quand le Driver se configure.

Ce plan le supprime et retire `.agents/backend/vm.md` au profit de `.agents/backend/jit.md`. Le titre de
l'issue #120 (`Epic(BackendVM)`) devient un abus de langage assumé. Le travail conceptuel de `vm.md` n'est pas
perdu : son seam `FunctionTable` et son modèle de session REPL sont repris en §4.4 et §4.5, transposés à ORC.

---

## 3. Phase 1 : Extraction de `BackendLlvmIr`

### 3.1 Inventaire

82 fichiers, 8 403 LOC dans `Private/`, plus `PchLLVM.hpp` (36), `meson.build` (68), et
`Public/Volt/BackendLLVM/LlvmEmitter.hpp` (149).

| Répertoire | Fichiers | LOC |
|---|---|---|
| `Core/` | 9 | 800 |
| `Functions/` | 14 | 1 423 |
| `Lower/` (racine) | 3 | 424 |
| `Lower/Expr/` | 20 | 2 665 |
| `Lower/Stmt/` | 6 | 500 |
| `Lower/Mono/` | 4 | 333 |
| `Lower/Closure/` | 4 | 283 |
| `Lower/Exception/` | 9 | 841 |
| `Types/` | 5 | 461 |
| `Target/` | 8 | 673 |

### 3.2 Table de migration

Chaque fichier apparaît **exactement une fois**. `LI` = `BackendLlvmIr`, `BC` = `BackendCore`,
`LL` = reste dans `BackendLLVM`.

#### `Core/` → 6 vers `LI`, 2 vers `BC`, 3 restent

| Fichier | LOC | → | Justification |
|---|---|---|---|
| `Core/ModuleContext.hpp` | 91 | **LI** | possède `LLVMContext`/`Module`/`IRBuilder`/`TargetMachine` — le substrat de l'émission |
| `Core/ModuleContext.cpp` | 82 | **LI** | idem |
| `Core/EmitterServices.hpp` | 144 | **LI** | bundle de pointeurs vers les services d'émission |
| `Core/LlvmFwd.hpp` | 41 | **LI** | discipline d'inclusion des headers privés d'émission |
| `Core/DiagnosticSink.hpp` | 77 | **BC** | zéro LLVM, canal d'erreur « premier échec gagne » utile aux 3 backends |
| `Core/DiagnosticSink.cpp` | 20 | **BC** | idem |
| `Core/LlvmBackend.cpp` | 65 | **LL** | câble le graphe de services AOT, ctor/dtor pimpl, `static_assert` |
| `Core/LlvmBackendState.hpp` | 94 | **LL** | corps du pimpl `LlvmBackend::State` — état de la queue AOT |
| `Core/LlvmLifecycle.cpp` | 186 | **LL** | `Begin`/`EmitUnit`/`Finalize` AOT : verify → optimize → `.o` → link |

#### `Functions/` → 14 vers `LI`

| Fichier | LOC | → | Justification |
|---|---|---|---|
| `Functions/FunctionRegistry.hpp` | 157 | **LI** | cache `symbole → llvm::Function*` + déclaration des 9 sweeps |
| `Functions/FunctionRegistry.cpp` | 89 | **LI** | linkage, attributs, `DeclareMember` |
| `Functions/SignatureBuilder.hpp` | 63 | **LI** | `FunctionTypeOf` rend un `llvm::FunctionType*` |
| `Functions/SignatureBuilder.cpp` | 121 | **LI** | ordre ABI (`self`, params, `env`) → `FunctionType`. La *décision* par-valeur/par-adresse est extraite en `BC::AbiClassifier` (§3.3) ; la *construction* reste ici |
| `Functions/ParameterBinder.hpp` | 45 | **LI** | `llvm::Value*` dans la signature |
| `Functions/ParameterBinder.cpp` | 79 | **LI** | slot d'argument, `bByAddress`, champ `def initialize(@x)` |
| `Functions/DeclareSweep.cpp` | 86 | **LI** | sweep piloté par le `TypeStore` — identique AOT/JIT |
| `Functions/DefineSweep.cpp` | 193 | **LI** | monte `FunctionFrame` + `BodyEmitter` par corps |
| `Functions/SynthesizedSweep.cpp` | 168 | **LI** | fonctions issues de `ClosureLifting`, `ptr %env` en queue |
| `Functions/UnitInitEmitter.cpp` | 62 | **LI** | `_V_init_<Ordinal>` via un vrai `BodyEmitter` |
| `Functions/EntryPointEmitter.cpp` | 148 | **LI** | `_V_init_all` + shim `main`. IR à la main, mais le JIT en a besoin aussi (§4.3) — gagne un mode « pas de shim CRT » |
| `Functions/SymbolNameEmitter.cpp` | 96 | **LI** | `_V_symbol_name`, `switch` sur la table de symboles du build |
| `Functions/VTableRegistry.hpp` | 44 | **LI** | cache `@_VTable_*`. **Nettoyage :** remplacer `#include <llvm/IR/GlobalVariable.h>` par `LlvmFwd.hpp` |
| `Functions/VTableRegistry.cpp` | 72 | **LI** | vtable = tableau constant de pointeurs de fonction |

#### `Lower/` racine → 3 vers `LI`

| Fichier | LOC | → | Justification |
|---|---|---|---|
| `Lower/BodyEmitter.hpp` | 188 | **LI** | le seam central ; 23 mentions `llvm::` dans ses signatures |
| `Lower/BodyEmitter.cpp` | 129 | **LI** | `MakeTemp` (alloca en bloc d'entrée), `CoerceWidth` |
| `Lower/FunctionFrame.hpp` | 107 | **LI** | ~80 % neutre, mais `Fn`/`Rescues`/`Entry`/`Self` sont LLVM |

#### `Lower/Expr/` → 18 vers `LI`, 2 scindés

| Fichier | LOC | → | Justification |
|---|---|---|---|
| `Expr/ExprEmitter.hpp` | 126 | **LI** | déclare ~24 bras, tous en `llvm::Value*` |
| `Expr/ExprEmitter.cpp` | 161 | **LI** | site `std::visit` + `EmitAssign`. **Nettoyage :** extraire le bras `DynamicUpcast` (seul bras inline, 4 `Create*`) en fonction nommée |
| `Expr/ExprPlaceEmitter.cpp` | 206 | **LI** | 2ᵉ site `std::visit` (4 place kinds) + `LoadPlace` + `FieldAddress` |
| `Expr/ExprLiteralEmitter.cpp` | 497 | **LI** | ~90 % décodage de lexème (neutre), mais 1 `Create*` et 26 `llvm::` — scinder coûterait plus qu'il ne rapporte ; à revoir si un backend VM revient |
| `Expr/ExprResolvedCallEmitter.cpp` | 344 | **LI** | le site d'appel résolu : récepteur, args, block, mono, check post-appel |
| `Expr/ExprCallEmitter.cpp` | 60 | **LI** | nœud `Call` → `EmitResolvedCall` |
| `Expr/ExprAccessEmitter.cpp` | 105 | **LI** | `x`, `@x`, `o.x`, `self`, `super`, `FuncAddr` |
| `Expr/ExprBinaryEmitter.cpp` | 113 | **LI** | 8 `Create*` |
| `Expr/ExprUnaryEmitter.cpp` | 73 | **LI** | 8 `Create*` |
| `Expr/ExprTernaryEmitter.cpp` | 101 | **LI** | seule convergence par `phi` (2 prédécesseurs fixes) |
| `Expr/ExprCaseEmitter.cpp` | 103 | **LI** | échelle `WhenClause` post-CaseLowering |
| `Expr/ExprIfEmitter.cpp` | 56 | **LI** | `if` en position de valeur |
| `Expr/ExprControlEmitter.cpp` | 54 | **LI** | `EmitConvergingBody`, forme partagée `if`/`case` |
| `Expr/ExprShortCircuit.cpp` | 55 | **LI** | `and`/`or` comme flot de contrôle + `phi` |
| `Expr/ExprPointerArith.cpp` | 55 | **LI** | `gep` avec foulée du pointé |
| `Expr/ExprStoreLoad.cpp` | 117 | **LI** | store scalaire vs `memcpy` dimensionné par `LayoutEngine` |
| `Expr/ExprIndirectCallEmitter.cpp` | 114 | **LI** | bras `bIndirect` |
| `Expr/ExprOperatorEmitter.cpp` | 40 | **LI** | zéro LLVM, mais couplé aux enums de `InstructionTables` — suit le schéma |
| `Expr/InstructionTables.hpp` | 124 | **scindé** | schéma (`EOpFamily`, `EUnaryOp`, `BinOpRow`, `CmpRow`, `UnOpRow`, `FamilyOf`) → **BC** avec des opcodes neutres ; le typage `llvm::Instruction::BinaryOps` / `llvm::CmpInst::Predicate` → **LI** |
| `Expr/InstructionTables.cpp` | 161 | **scindé** | le manifeste `Instructions.inl` (famille × `TokenKind` → `EBinOp`/`ECmpPred`/`EUnaryOp`) → **BC** ; la table `EBinOp → llvm::Instruction::BinaryOps` (~40 lignes) → **LI** |

Le scindage de `InstructionTables` est le seul cas où on paie une petite indirection pour un gain futur : il
rend le manifeste réellement partageable (`rules/meta-first.md` cite `Bytecode.inl` comme le patron), donc un
backend WASM ajoute une colonne au lieu de recopier les lignes.

#### `Lower/Stmt/` → 6 vers `LI`

| Fichier | LOC | → | Justification |
|---|---|---|---|
| `Stmt/StmtEmitter.hpp` | 59 | **LI** | zéro type LLVM, mais déclare des fonctions prenant `BodyEmitter&` |
| `Stmt/StmtEmitter.cpp` | 74 | **LI** | site `std::visit` statements + `EmitExprStmt` (le seul site `ret`) |
| `Stmt/StmtLocalDeclEmitter.cpp` | 108 | **LI** | `SlotFor` : alloca local **ou global de module** — central pour le REPL (§4.5) |
| `Stmt/StmtLoopEmitter.cpp` | 48 | **LI** | `while`, `LoopFrame` |
| `Stmt/StmtReturnBreakNext.cpp` | 116 | **LI** | `return`/`break`/`next`, chemin empoisonné |
| `Stmt/TailValue.cpp` | 95 | **LI** | `StoreTailValue`/`LoadConverged` |

#### `Lower/Mono/` → 2 vers `LI`, 2 vers `BC`

| Fichier | LOC | → | Justification |
|---|---|---|---|
| `Mono/MonoDriver.hpp` | 76 | **LI** | déclare `EmitMonomorphizedBody` — c'est la partie LLVM |
| `Mono/MonoBodyEmitter.cpp` | 186 | **LI** | overlay `ReinstantiateBody` → frame → `BodyEmitter` |
| `Mono/MonoDriver.cpp` | 25 | **BC** | boucle de drain jusqu'au point fixe : pur ordonnancement. Rejoint `Monomorphizer` sous le nom `MonoQueue::Drain` |
| `Mono/MonoLookup.cpp` | 46 | **BC** | résout `MonoRequest` → `Member` + `UnitView`. Zéro LLVM, lecture du `TypeStore` |

#### `Lower/Closure/` → 4 vers `LI`

| Fichier | LOC | → | Justification |
|---|---|---|---|
| `Closure/ClosureLowering.hpp` | 68 | **LI** | `llvm::StructType*`, `llvm::Value*` |
| `Closure/ClosurePairType.cpp` | 18 | **LI** | construit `{ ptr code, ptr env }` — les *indices* viennent déjà de `BC::ClosureABI` |
| `Closure/IndirectCallEmitter.cpp` | 152 | **LI** | charge la paire, reconstruit le `FunctionType`, appel indirect |
| `Closure/BlockNextEmitter.cpp` | 45 | **LI** | `next` dans un corps lifté → `ret` |

#### `Lower/Exception/` → 9 vers `LI`

| Fichier | LOC | → | Justification |
|---|---|---|---|
| `Exception/ExceptionLowering.hpp` | 157 | **LI** | 10 `llvm::GlobalVariable*` en état. Gagne `ETlsAccess` et `bDefineGlobals` (§4.2) |
| `Exception/BeginRescueEmitter.cpp` | 264 | **LI** | fichier le plus dense en IR du module (22 `Create*`) |
| `Exception/ExceptionChecks.cpp` | 81 | **LI** | `EmitExceptionCheck`/`EmitUnwindCheck` — scinde le bloc courant |
| `Exception/RaiseEmitter.cpp` | 80 | **LI** | copie dans le stockage TLS, publie adresse + tag |
| `Exception/ExceptionGlobals.cpp` | 64 | **LI** | crée les 3 slots TLS. **Point d'entrée du mode `Accessor`** (§4.2) |
| `Exception/ExceptionStorage.cpp` | 62 | **LI** | dimensionne le tampon via `LayoutEngine` |
| `Exception/PreorderTable.cpp` | 40 | **LI** | `NominalId → PreorderLeft` en tableau constant |
| `Exception/AncestorTest.cpp` | 42 | **LI** | test d'intervalle d'Euler en O(1) |
| `Exception/PoisonedPath.cpp` | 51 | **LI** | branche vers `Frame.Rescues.back()` ou retour anticipé |

#### `Types/` → 4 vers `LI`, 1 vers `BC`

| Fichier | LOC | → | Justification |
|---|---|---|---|
| `Types/TypeMapper.hpp` | 95 | **LI** | `LayoutId → llvm::Type*` |
| `Types/TypeMapper.cpp` | 192 | **LI** | construction Primitive/Pointer/Aggregate + cache |
| `Types/AbiVerifier.hpp` | 46 | **LI** | croise `LayoutEngine` et `DataLayout` — le JIT a aussi un `DataLayout` (celui de `LLJIT`), donc le contrôle vaut pour les deux |
| `Types/AbiVerifier.cpp` | 48 | **LI** | idem |
| `Types/LayoutOfValue.cpp` | 80 | **BC** | `FlattenValueType` : `SemaTypeId → LayoutId`, pur, zéro LLVM. Rejoint `InstanceLayout.cpp` |

#### `Target/` → 8 restent

| Fichier | LOC | → | Justification |
|---|---|---|---|
| `Target/TargetPipeline.hpp` | 67 | **LL** | verify / optimize / `.ll` / `.o` — la queue AOT par définition |
| `Target/ModuleVerifier.cpp` | 46 | **LL** | `verifyModule`. Le JIT a son propre point de vérification (§4.3) |
| `Target/Optimizer.cpp` | 65 | **LL** | `PassBuilder`. Le JIT en M1 tourne à O0 ; M5 réutilisera cette logique via `BC` si besoin |
| `Target/IrEmitter.cpp` | 27 | **LL** | `--emit ir` |
| `Target/ObjectEmitter.cpp` | 40 | **LL** | `addPassesToEmitFile` |
| `Target/LinkerDriver.hpp` | 60 | **LL** | le JIT ne lie rien |
| `Target/LinkerDriver.cpp` | 211 | **LL** | pilote `cc` (mold → LLD) |
| `Target/StdlibArtifact.cpp` | 157 | **LL** | produit le `.a`/`.so` de la stdlib. **Le JIT le consomme, ne le duplique pas** (§5) |

#### Racine et `Public/`

| Fichier | LOC | → | Justification |
|---|---|---|---|
| `PchLLVM.hpp` | 36 | **déplacé** en `source/Volt/Backend/PchLLVM.hpp` — partagé par les 3 modules LLVM |
| `meson.build` | 68 | **scindé** — le bloc `llvm_dep` remonte en `meson/meson.build` (§6.3) |
| `Public/Volt/BackendLLVM/LlvmEmitter.hpp` | 149 | **LL**, inchangé — l'API publique de `volt build` ne bouge pas |

**Bilan :** `LI` ≈ 55 fichiers / ~6 900 LOC · `BC` +6 fichiers / ~250 LOC · `LL` ≈ 15 fichiers / ~1 250 LOC.

### 3.3 `BackendCore` après migration

```
BackendCore/
├── Public/Volt/BackendCore/
│   ├── BackendInput.hpp          (inchangé)
│   ├── TargetBackend.hpp         (inchangé)
│   ├── ExecutableBackend.hpp     ★ IJitBackend
│   ├── LayoutEngine.hpp          (inchangé)
│   ├── AbiClassifier.hpp         ★ par-valeur / par-adresse depuis LayoutId
│   ├── Instructions.inl          ★ manifeste famille × TokenKind → opcode neutre
│   ├── InstructionSchema.hpp     ★ EOpFamily, EBinOp, ECmpPred, EUnaryOp, FindBinOp...
│   ├── DiagnosticSink.hpp        ★ déplacé
│   ├── Mangler.hpp               (inchangé)
│   ├── SymbolRegistry.hpp        (inchangé)
│   ├── InstanceLayout.hpp        (inchangé)
│   ├── Monomorphizer.hpp         + MonoQueue::Drain, MonoLookup
│   ├── VTableLayout.hpp          (inchangé)
│   ├── InitAllSynthesizer.hpp    (inchangé)
│   ├── UnwindTransport.hpp       + SlotAccessorSymbol
│   └── ClosureABI.hpp            (inchangé)
└── Private/
    ├── DiagnosticSink.cpp        ★
    ├── AbiClassifier.cpp         ★
    ├── InstructionSchema.cpp     ★
    ├── MonoQueue.cpp             ★ (ex MonoDriver.cpp + MonoLookup.cpp)
    ├── LayoutOfValue.cpp         ★ déplacé
    └── (les 6 existants, inchangés)
```

Les trois ajouts d'interface :

```cpp
// BackendCore/Public/Volt/BackendCore/AbiClassifier.hpp
namespace Volt::Backend
{
    // Comment un paramètre ou un retour de cette forme voyage. Décidé depuis
    // le LayoutNode seul (abi.md : « in by pointer, out by value ») — aucun
    // nom Volt, aucun type de backend. Les trois cibles lisent la même réponse.
    enum class EParamClass : std::uint8_t
    {
        Scalar    = 0, // registre / slot de pile
        ByAddress = 1, // agrégat : le paramètre est l'adresse du stockage
    };

    [[nodiscard]] BACKENDCORE_EXPORT EParamClass
    ClassifyParam ( const MiddleEnd::TypeSystem::TypeStore &Store, MiddleEnd::TypeSystem::LayoutId Id );

    [[nodiscard]] BACKENDCORE_EXPORT bool
    IsAggregate ( const MiddleEnd::TypeSystem::TypeStore &Store, MiddleEnd::TypeSystem::LayoutId Id );
}
```

```cpp
// BackendCore/Public/Volt/BackendCore/UnwindTransport.hpp   (ajout)
struct BACKENDCORE_EXPORT UnwindTransport
{
    static constexpr std::string_view ExceptionValueSlot = "volt.exc.value";
    static constexpr std::string_view ExceptionTagSlot   = "volt.exc.tag";
    static constexpr std::string_view BreakFlagSlot      = "volt.brk.flag";
    static constexpr std::uint32_t NoExceptionTag        = 0xFFFFFFFF;

    // ★ Accesseur non-TLS des trois slots, pour les cibles où une relocation
    // thread-local n'est pas disponible (JIT : voir jit.md § « TLS »). Rend
    // l'adresse du bloc de slots du thread courant. Nommé ici, une fois, pour
    // qu'aucun backend n'invente son propre symbole (rules/zero-hardcode.md).
    // Disposition du bloc : { ptr Value; u32 Tag; u8 BreakFlag; } — offsets par
    // LayoutEngine sur le LayoutNode que SlotBlockLayout() rend.
    static constexpr std::string_view SlotAccessorSymbol = "__volt_unwind_slots";
};
```

```cpp
// BackendCore/Public/Volt/BackendCore/ExecutableBackend.hpp
namespace Volt::Backend
{
    struct RunResult
    {
        bool bOk          = false;
        std::int32_t Code = 0;   // code de sortie du programme Volt
        std::string Message;     // non vide seulement si bOk == false
    };

    enum class EReloadStatus : std::uint8_t
    {
        Ok        = 0,
        Refused   = 1, // signature/layout incompatible — message explique, redémarrage requis
        Error     = 2,
    };

    struct ReloadResult
    {
        EReloadStatus Status = EReloadStatus::Error;
        std::string Message;
        std::size_t PatchedSymbols = 0;
    };

    // Un backend qui exécute au lieu d'écrire un artefact. Étend le contrat
    // TargetBackend : Begin/EmitUnit/Finalize préparent, Run exécute.
    // Zéro type de toolchain ici — le Driver inclut ce header, jamais un
    // header LLVM (Driver.hpp : « Driver est le seul endroit où --target se
    // résout en IBackend concret »).
    class IJitBackend : public IBackend
    {
    public:
        ~IJitBackend () override = default;

        // Exécute le point d'entrée. Appelable une seule fois par session en
        // mode `run` ; le REPL utilise EvalUnit à la place.
        [[nodiscard]] virtual RunResult Run ( std::span<const std::string_view> ProgramArgs ) = 0;

        // Recompile une unité déjà émise et repointe ses symboles.
        // Exige que la session ait été construite avec ELinkage::Indirect.
        [[nodiscard]] virtual ReloadResult Reload ( const UnitView &Unit ) = 0;

        // Compile une unité incrémentale et exécute son initialiseur.
        // Le REPL appelle ceci une fois par ligne.
        [[nodiscard]] virtual RunResult EvalUnit ( const UnitView &Unit ) = 0;

        // Adresse d'un symbole manglé, pour les tests et le débogage.
        [[nodiscard]] virtual std::uintptr_t LookupSymbol ( std::string_view Mangled ) = 0;
    };
}
```

### 3.4 Deux changements de forme obligatoires

**1 — `ModuleContext` doit posséder son contexte par pointeur.**
Aujourd'hui `llvm::LLVMContext Ctx;` est un membre **par valeur** (`ModuleContext.hpp`). `orc::ThreadSafeModule`
exige de prendre possession du contexte (`ThreadSafeModule(unique_ptr<Module>, unique_ptr<LLVMContext>)`).

```cpp
// avant
llvm::LLVMContext Ctx;
// après
std::unique_ptr<llvm::LLVMContext> Ctx;   // jamais nul après InitTarget
```

**2 — La cible devient un paramètre, le `TargetMachine` devient optionnel.**
L'AOT dérive tout du triple hôte. Le JIT reçoit triple et `DataLayout` de `LLJIT`, et n'a **pas** besoin d'un
`TargetMachine` (il ne fait pas `addPassesToEmitFile`).

```cpp
struct TargetSpec
{
    std::string Triple;          // vide -> llvm::sys::getDefaultTargetTriple()
    std::string DataLayout;      // vide -> dérivé du TargetMachine créé
    bool bNeedTargetMachine = true;   // AOT: true. JIT: false.
};

[[nodiscard]] bool InitTarget ( std::string_view ModuleName, const TargetSpec &Spec, std::string &OutError );
[[nodiscard]] llvm::TargetMachine *MachinePtr () noexcept;   // nul en mode JIT
```

### 3.5 Les deux en-têtes publics de `BackendLlvmIr`

```cpp
// BackendLlvmIr/Public/Volt/BackendLlvmIr/IrGenerator.hpp
// ZÉRO header LLVM. Consommable par n'importe qui, y compris un outil.
namespace Volt::Backend::Ir
{
    enum class EModuleGranularity : std::uint8_t
    {
        Whole   = 0, // un llvm::Module pour tout le build (AOT, et JIT en M1)
        PerUnit = 1, // un module par unité (JIT : hot reload, REPL)
    };

    enum class ETlsAccess : std::uint8_t
    {
        Direct   = 0, // globals thread_local adressés directement (AOT)
        Accessor = 1, // appel à UnwindTransport::SlotAccessorSymbol (JIT)
    };

    enum class ELinkage : std::uint8_t
    {
        Direct   = 0, // call @sym
        Indirect = 1, // load ptr @volt.fn.sym puis call — seam de hot reload
    };

    struct IrOptions
    {
        EModuleGranularity Granularity = EModuleGranularity::Whole;
        ETlsAccess Tls                 = ETlsAccess::Direct;
        ELinkage Linkage               = ELinkage::Direct;
        // Ordinaux < SkipUnitsBelow : déclarés, jamais définis (stdlib pré-compilée).
        std::uint32_t SkipUnitsBelow = 0;
        std::string TargetTriple;    // vide -> hôte
        std::string DataLayout;      // vide -> dérivé
        std::string EntryFunction = "__volt_entry";
        std::string EntrySymbol   = "main";   // vide -> pas de shim CRT (JIT, lib)
        bool bVerify    = true;
        bool bDebugInfo = true;
    };

    class BACKENDLLVMIR_EXPORT IrGenerator
    {
    public:
        explicit IrGenerator ( IrOptions Options );
        ~IrGenerator ();
        IrGenerator ( IrGenerator && ) noexcept;
        IrGenerator &operator=( IrGenerator && ) noexcept;

        void Begin ( const BackendInput &Input );
        [[nodiscard]] EEmitStatus EmitUnit ( const UnitView &Unit );

        // Draine la queue de monomorphisation, émet _V_init_all / le point
        // d'entrée / la table de noms de symboles, vérifie. Après cet appel le
        // ou les modules sont complets.
        [[nodiscard]] EEmitStatus Finish ();

        // En mode PerUnit : émet le module prélude (globals partagés, _V_init_all,
        // point d'entrée). À appeler après tous les EmitUnit.
        [[nodiscard]] EEmitStatus EmitPrelude ();

        [[nodiscard]] bool Failed () const noexcept;
        [[nodiscard]] std::string_view Error () const noexcept;

        // Symboles définis par la dernière unité émise — le JIT s'en sert pour
        // savoir quels slots d'indirection repointer (§4.4).
        [[nodiscard]] std::vector<std::string> LastUnitSymbols () const;

        struct State;
    private:
        std::unique_ptr<State> Impl;
    };
}
```

```cpp
// BackendLlvmIr/Public/Volt/BackendLlvmIr/LlvmAccess.hpp
// CONSOMMATEURS LLVM-AWARE UNIQUEMENT — BackendLLVM et BackendJIT.
// Séparé d'IrGenerator.hpp pour que le Driver puisse inclure celui-là sans
// tirer LLVM (rules/shared-lib-exports.md, et la discipline de LlvmEmitter.hpp).
#include <llvm/IR/Module.h>
#include <llvm/IR/LLVMContext.h>

namespace Volt::Backend::Ir
{
    struct OwnedModule
    {
        std::unique_ptr<llvm::LLVMContext> Context;
        std::unique_ptr<llvm::Module> Module;
    };

    // Mode Whole : le module unique. Mode PerUnit : le prélude.
    [[nodiscard]] BACKENDLLVMIR_EXPORT OwnedModule TakeModule ( IrGenerator &Gen );

    // Mode PerUnit : les modules d'unité, dans l'ordre d'émission (donc l'ordre
    // de lien du circuit). Vide en mode Whole.
    [[nodiscard]] BACKENDLLVMIR_EXPORT std::vector<OwnedModule> TakeUnitModules ( IrGenerator &Gen );

    // Emprunt non-possédant, pour la queue AOT qui optimise puis émet sur place.
    [[nodiscard]] BACKENDLLVMIR_EXPORT llvm::Module &ModuleOf ( IrGenerator &Gen );
    [[nodiscard]] BACKENDLLVMIR_EXPORT llvm::TargetMachine *MachineOf ( IrGenerator &Gen );
}
```

### 3.6 Critère de sortie de la Phase 1

**`volt build` inchangé, suites `golden` et `samples` vertes.** Aucune signature d'émission ne change ;
c'est un déplacement plus les deux changements de forme de §3.4. Commit isolé, sans aucun code JIT.

---

## 4. Phase 2 : Implémentation `BackendJIT`

### 4.1 Composants

| Composant | Fichiers | Responsabilité |
|---|---|---|
| `JitBackend` | `JitBackend.cpp` + `Public/.../JitBackend.hpp` | implémente `TargetBackend` **et** `IJitBackend` ; pimpl |
| `JitCompiler` | `JitCompiler.{hpp,cpp}` | possède `orc::LLJIT`, les `ResourceTracker`, ajoute des `ThreadSafeModule` |
| `JitModuleBuilder` | `JitModuleBuilder.{hpp,cpp}` | pilote `Ir::IrGenerator` en mode `PerUnit`, rend des `OwnedModule` |
| `JitSymbolResolver` | `JitSymbolResolver.{hpp,cpp}` | générateurs de symboles : stdlib `.so`, processus courant, symboles absolus |
| `JitStdlibLoader` | `JitStdlibLoader.{hpp,cpp}` | localise/valide le `.so` de stdlib, l'attache à la JITDylib |
| `JitRuntime` | `JitRuntime.{hpp,cpp}` | table d'indirection, générations, arène de session, patch |
| `JitEntryPoint` | `JitEntryPoint.{hpp,cpp}` | résout et appelle `__volt_entry` / l'initialiseur d'une ligne REPL |

### 4.2 Interfaces C++

```cpp
// Public/Volt/BackendJIT/JitBackend.hpp — ZÉRO header LLVM
#pragma once
#include "BackendJIT_export.hpp"
#include "Volt/BackendCore/ExecutableBackend.hpp"
#include <memory>
#include <string>
#include <string_view>

namespace Volt::Backend::Jit
{
    struct JitOptions
    {
        // Chemin du .so de stdlib pré-compilé. Vide -> aucune stdlib chargée
        // (build --no-stdlib) ; le prélude définit alors lui-même les globals.
        std::string StdlibPath;

        // Nombre d'unités de tête à ne PAS émettre (elles vivent dans le .so).
        std::uint32_t SkipUnitsBelow = 0;

        // Indirection : requise pour Reload, inutile sinon. `volt run` nu la
        // laisse à false et paie zéro surcoût d'appel.
        bool bIndirectLinkage = false;

        // PerUnit est requis pour Reload et pour le REPL. `volt run` nu peut
        // rester en Whole, qui produit un seul module et donc un seul passage
        // de compilation ORC.
        bool bPerUnitModules = false;

        std::uint8_t OptLevel        = 0;
        unsigned CompileThreads      = 0;   // 0 -> compilation synchrone
        std::string EntryFunction    = "__volt_entry";
    };

    class BACKENDJIT_EXPORT JitBackend final : public IJitBackend
    {
    public:
        JitBackend ();
        ~JitBackend () override;
        JitBackend ( JitBackend && ) noexcept;
        JitBackend &operator=( JitBackend && ) noexcept;

        void SetOptions ( JitOptions Options );

        // --- TargetBackend ---------------------------------------------------
        [[nodiscard]] std::string_view Name () const override { return "jit"; }
        void Begin ( const BackendInput &Input ) override;
        [[nodiscard]] EEmitStatus EmitUnit ( const UnitView &Unit ) override;
        [[nodiscard]] EmitResult Finalize () override;

        // --- IJitBackend -----------------------------------------------------
        [[nodiscard]] RunResult Run ( std::span<const std::string_view> ProgramArgs ) override;
        [[nodiscard]] ReloadResult Reload ( const UnitView &Unit ) override;
        [[nodiscard]] RunResult EvalUnit ( const UnitView &Unit ) override;
        [[nodiscard]] std::uintptr_t LookupSymbol ( std::string_view Mangled ) override;

    private:
        struct State;
        std::unique_ptr<State> Impl;
    };
}
```

`JitBackend` déclare des `virtual` (via `IJitBackend`) là où les autres backends n'en déclarent pas. C'est
délibéré et sans coût : le contrat `TargetBackend` reste satisfait par les quatre mêmes méthodes, et le
`static_assert` le prouve à la compilation ; les virtuels n'existent que pour `Run`/`Reload`/`EvalUnit`, qui
sont appelés une fois par exécution, pas une fois par nœud. `core-interfaces.md` autorise exactement ce
niveau — *« un saut virtuel par unité, jamais par nœud »*.

```cpp
// Private/JitCompiler.hpp
namespace Volt::Backend::Jit
{
    // Une génération = un lot de modules ajouté ensemble, retirable ensemble.
    // Une unité rechargée en crée une nouvelle ; une ligne de REPL aussi.
    using GenerationId = std::uint32_t;

    class JitCompiler
    {
    public:
        [[nodiscard]] bool Init ( const JitOptions &Options, std::string &OutError );

        [[nodiscard]] std::string TargetTriple () const;
        [[nodiscard]] std::string DataLayoutString () const;

        // Nouvelle génération vide. Les modules ajoutés ensuite lui appartiennent.
        [[nodiscard]] GenerationId OpenGeneration ();

        [[nodiscard]] bool AddModule ( GenerationId Gen, Ir::OwnedModule Module, std::string &OutError );

        // Retire une génération : démappe son code. INTERDIT si une frame peut
        // encore y être. Utilisé seulement par le REPL sur une ligne close (§4.5).
        [[nodiscard]] bool DropGeneration ( GenerationId Gen, std::string &OutError );

        // Force la matérialisation et rend l'adresse. Erreur si non résolu.
        [[nodiscard]] bool Lookup ( std::string_view Mangled, std::uintptr_t &OutAddr, std::string &OutError );

        // Définit des symboles à adresse fixe (arène de session REPL, §4.5).
        [[nodiscard]] bool DefineAbsolute ( std::span<const std::pair<std::string, std::uintptr_t>> Symbols,
                                            std::string &OutError );

        // Attache un générateur de recherche sur une bibliothèque partagée.
        [[nodiscard]] bool AddDylib ( std::string_view Path, std::string &OutError );

        // Attache le processus courant (libc, libm, et le compilateur lui-même
        // pour __volt_unwind_slots en build --no-stdlib).
        [[nodiscard]] bool AddProcessSymbols ( std::string &OutError );

    private:
        struct Impl;
        std::unique_ptr<Impl> P;   // possède orc::LLJIT + la map GenerationId -> ResourceTrackerSP
    };
}
```

API ORC utilisée, toutes vérifiées présentes en **LLVM 22.1.8** (le seul LLVM du dépôt, consommé en `.so`
monolithique — cf. §6.3) :

| Appel | En-tête |
|---|---|
| `orc::LLJITBuilder{}.setNumCompileThreads(n).create()` | `Orc/LLJIT.h` |
| `LLJIT::getMainJITDylib()` | `Orc/LLJIT.h` |
| `JITDylib::createResourceTracker()` → `ResourceTrackerSP` | `Orc/Core.h` |
| `LLJIT::addIRModule(ResourceTrackerSP, ThreadSafeModule)` → `Error` | `Orc/LLJIT.h` |
| `ResourceTracker::remove()` → `Error` | `Orc/Core.h` |
| `LLJIT::lookup(JITDylib&, StringRef)` → `Expected<ExecutorAddr>` | `Orc/LLJIT.h` |
| `DynamicLibrarySearchGenerator::Load(File, GlobalPrefix, Allow)` | `Orc/ExecutionUtils.h` |
| `DynamicLibrarySearchGenerator::GetForCurrentProcess(GlobalPrefix)` | `Orc/ExecutionUtils.h` |
| `orc::absoluteSymbols(SymbolMap)` | `Orc/AbsoluteSymbols.h` |
| `ThreadSafeModule(unique_ptr<Module>, unique_ptr<LLVMContext>)` | `Orc/ThreadSafeModule.h` |

```cpp
// Private/JitRuntime.hpp
namespace Volt::Backend::Jit
{
    // Le seam de rechargement, transposé du FunctionTable de vm.md.
    //
    // En ELinkage::Indirect, tout appel inter-unité passe par un global
    //     @volt.fn.<mangled> = ptr
    // que le prélude définit et que JitRuntime écrit. Patcher = un store.
    // Le code ancien n'est JAMAIS démappé, donc une frame vivante finit son
    // exécution sur l'ancienne version — exactement la garantie de vm.md.
    class JitRuntime
    {
    public:
        explicit JitRuntime ( JitCompiler &InCompiler ) : Compiler( &InCompiler ) {}

        // Après matérialisation d'une génération : pour chaque symbole défini,
        // résout l'adresse réelle et l'écrit dans son slot d'indirection.
        [[nodiscard]] bool PublishSymbols ( std::span<const std::string> Mangled, std::string &OutError );

        // Empreinte d'une unité, pour refuser un reload incompatible (§4.4).
        struct UnitSignature
        {
            // mangled -> LayoutId aplati de la signature (params + retour)
            std::map<std::string, std::vector<std::uint32_t>> Functions;
            // NominalId -> LayoutId de l'instance
            std::map<std::uint32_t, std::uint32_t> Layouts;
        };

        [[nodiscard]] static UnitSignature SignatureOf ( const BackendInput &Build, const UnitView &Unit );

        // Rend un message non vide si le rechargement doit être refusé.
        [[nodiscard]] static std::string DiffRefusal ( const UnitSignature &Old, const UnitSignature &New );

        void RecordUnit ( std::uint32_t Ordinal, UnitSignature Sig, GenerationId Gen );
        [[nodiscard]] const UnitSignature *SignatureFor ( std::uint32_t Ordinal ) const;

        // Arène de session REPL : mémoire hôte, adresses stables, survit au
        // retrait de n'importe quelle génération.
        [[nodiscard]] std::uintptr_t AllocateBinding ( std::string_view Mangled, std::size_t Size, std::size_t Align );

    private:
        JitCompiler *Compiler = nullptr;
        std::map<std::uint32_t, std::pair<UnitSignature, GenerationId>> Units;
        std::vector<std::unique_ptr<std::byte[]>> Arena;   // chunks, jamais réalloués
        std::map<std::string, std::uintptr_t> Bindings;
    };
}
```

### 4.2.1 TLS : pourquoi le JIT n'adresse pas les globals thread-local directement

Le transport d'exceptions repose sur trois globals `thread_local` (`UnwindTransport.hpp`). Du code JIT-é qui
les adresse directement émet des relocations TLS, que JITLink ne résout qu'avec `ELFNixPlatform` **plus**
l'archive runtime `liborc_rt`.

État constaté sur cette machine : `liborc_rt-x86_64.a` n'existe qu'en **compiler-rt 21.1.8**, alors que LLVM
est en **22.1.8**. Le protocole de bootstrap ORC-RT ↔ `ELFNixPlatform` est versionné et change entre versions
majeures ; faire reposer le MVP dessus, c'est faire reposer le MVP sur une dépendance désalignée qu'on ne
contrôle pas.

**Décision : le JIT utilise `ETlsAccess::Accessor` par défaut.** `ExceptionGlobals.cpp` gagne un branchement :

- `Direct` (AOT, comportement actuel) — crée/adresse les `llvm::GlobalVariable` `thread_local` ;
- `Accessor` (JIT) — déclare `ptr @__volt_unwind_slots()` et adresse les trois slots par `gep` sur le bloc
  qu'il rend, avec des offsets calculés par `LayoutEngine` — donc pas de décalage codé en dur.

Conséquences : aucune relocation TLS dans le code JIT-é, pas d'`ELFNixPlatform`, pas de `liborc_rt`. Un `LLJIT`
nu avec `DynamicLibrarySearchGenerator` suffit. Le coût est un appel de fonction par accès aux slots, sur un
chemin déjà dominé par un test-et-branchement — négligeable, et payé seulement par le JIT.

L'implémentation de `__volt_unwind_slots` est du code natif ordinaire (où le TLS fonctionne) et vit dans la
stdlib `.so`. En build `--no-stdlib`, elle est fournie par le compilateur lui-même, atteinte via
`AddProcessSymbols()`. Le nom est déclaré une seule fois, dans `BackendCore` — aucun backend n'invente de
symbole, `rules/zero-hardcode.md` est respecté. Ce n'est pas non plus un contournement de
`rules/backend-machine-only.md` : les slots d'unwind sont déjà de la machinerie appartenant au backend
(`UnwindTransport.hpp` les spécifie), pas une construction de niveau Volt.

### 4.3 Algorithme de compilation unitaire

Le point dur de cette phase, à traiter comme tel : **`BackendLLVM` construit UN SEUL `llvm::Module` pour tout
le build**. Le hot reload et le REPL exigent **un module par unité**. D'où `EModuleGranularity`.

En mode `PerUnit`, trois choses changent dans `BackendLlvmIr` :

1. le cache `FunctionRegistry` (`symbole → llvm::Function*`) est **réinitialisé à chaque module** — sinon un
   `llvm::Function*` d'un module fuit dans le suivant ;
2. les globals partagés — les trois slots d'unwind, `volt.exc.storage`, la table de préordre, la table de noms
   de symboles, `_V_init_all`, `__volt_entry`, et les slots `@volt.fn.*` — appartiennent à un **module
   prélude** unique. `ExceptionLowering` gagne `bDefineGlobals` : vrai pour le prélude, faux ailleurs
   (déclaration externe) ;
3. les globals de module issus d'un `LocalDecl` de haut niveau (`StmtLocalDeclEmitter.cpp`) sont définis dans
   le module de leur unité et déclarés externes ailleurs.

**En pratique, avec la stdlib `.so` chargée, c'est elle qui définit ces globals** — elle contient déjà des
`raise`. Le prélude ne fait que les déclarer, et le générateur de dylib les résout. Le cas « qui définit » ne
se pose que pour un build `--no-stdlib`.

```
JitBackend::Begin( Input )
 1. Impl->Build = &Input
 2. JitCompiler::Init( Options )
      LLJITBuilder{}.setNumCompileThreads( Options.CompileThreads ).create()
      -> triple + DataLayout viennent de LLJIT, pas d'un TargetMachine
 3. JitStdlibLoader::Attach()
      si Options.StdlibPath non vide :
          JitCompiler::AddDylib( StdlibPath )        // DynamicLibrarySearchGenerator::Load
      JitCompiler::AddProcessSymbols()                // libc, libm, __volt_unwind_slots
 4. JitModuleBuilder::Begin( Input ) avec IrOptions{
        Granularity   = Options.bPerUnitModules ? PerUnit : Whole,
        Tls           = Accessor,
        Linkage       = Options.bIndirectLinkage ? Indirect : Direct,
        SkipUnitsBelow = Options.SkipUnitsBelow,      // == BackendInput::StdlibUnitCount
        TargetTriple  = JitCompiler::TargetTriple(),
        DataLayout    = JitCompiler::DataLayoutString(),
        EntrySymbol   = "" }                          // pas de shim CRT : on appelle __volt_entry
      -> DeclareAll : déclare TOUT le build, stdlib comprise, dans le module courant

JitBackend::EmitUnit( Unit )
 5. si Unit.Ordinal < Options.SkipUnitsBelow -> return Ok      // la stdlib est dans le .so
 6. IrGenerator::EmitUnit( Unit )                              // DefineAll -> BodyEmitter
 7. si PerUnit :
        Module = TakeUnitModules().back()
        Gen    = JitCompiler::OpenGeneration()
        JitCompiler::AddModule( Gen, move(Module) )
        JitRuntime::RecordUnit( Unit.Ordinal, SignatureOf( Build, Unit ), Gen )

JitBackend::Finalize()
 8. IrGenerator::Finish()      // draine la mono, _V_init_all, table de symboles, verify
 9. si PerUnit : IrGenerator::EmitPrelude() puis AddModule( PreludeGen, TakeModule() )
    sinon        : AddModule( WholeGen, TakeModule() )
10. si Indirect : JitRuntime::PublishSymbols( tous les symboles définis )
11. return EmitResult{ Ok, Artifact = "<jit>", "" }
    // `Artifact` est le nom que TargetBackend impose ; il n'y a pas de fichier.

JitBackend::Run( ProgramArgs )
12. JitEntryPoint::Resolve( Options.EntryFunction )    // LLJIT::lookup -> ExecutorAddr
13. reinterpret_cast<std::int32_t(*)()>( Addr )        // signature de __volt_entry
14. appel, capture du code de retour
15. Prelude.vl gère lui-même une exception non rattrapée par son begin/rescue ;
    rien à faire de spécial côté hôte.
```

La monomorphisation traverse les modules sans changement : la queue est globale au build
(`BackendCore::Monomorphizer` dédoublonne sur `Key()`), et une instanciation est émise dans le module de
l'unité qui la demande en premier. Les autres modules la voient comme une déclaration externe.

### 4.4 Algorithme de hot reload

Prérequis : `bPerUnitModules = true` **et** `bIndirectLinkage = true`. Activé par `volt run --watch`.

En `ELinkage::Indirect`, `ExprResolvedCallEmitter` émet, pour tout appel dont le callee est défini dans une
autre unité :

```llvm
; au lieu de :  %r = call i32 @_V4Math4sqrt(double %x)
%fp = load ptr, ptr @volt.fn._V4Math4sqrt
%r  = call i32 %fp(double %x)
```

Le prélude définit `@volt.fn.<mangled> = global ptr null` pour chaque symbole du build. Les appels
intra-unité restent directs — ils sont rechargés en bloc avec leur unité, donc l'indirection n'y apporte rien.

```
JitBackend::Reload( Unit )
 1. si non ( PerUnit et Indirect ) :
        return Refused{ "hot reload exige --watch (modules par unité + liaison indirecte)" }

 2. NewSig = JitRuntime::SignatureOf( *Build, Unit )
    OldSig = JitRuntime::SignatureFor( Unit.Ordinal )
    si OldSig == nullptr : return Error{ "unité jamais émise" }

 3. Refusal = JitRuntime::DiffRefusal( *OldSig, NewSig )
    si Refusal non vide : return Refused{ Refusal }

    DiffRefusal refuse, exactement comme vm.md l'exigeait :
      - une fonction dont la signature aplatie a changé ET qui a une frame vivante
        (le JIT ne pouvant pas inspecter la pile, M3 refuse dès qu'une signature
         change, sans condition de frame — plus strict, jamais faux) ;
      - un nominal dont le LayoutId a changé et dont des instances peuvent exister
        (c'est-à-dire dès qu'il est mentionné par une unité déjà exécutée).
    Repli dans les deux cas : redémarrage complet, que cette cible rend bon marché.

 4. Gen = JitCompiler::OpenGeneration()                 // NOUVELLE génération
    Module = JitModuleBuilder::RebuildUnit( Unit )      // un module neuf pour cette unité
    JitCompiler::AddModule( Gen, move(Module) )

    L'ANCIENNE génération n'est PAS retirée. C'est le point crucial :
    ResourceTracker::remove() démapperait la mémoire exécutable, et une frame
    vivante s'y exécutant partirait en SIGSEGV. On paie de la mémoire résidente
    (quelques dizaines de Ko par rechargement) contre la correction. Une session
    --watch qui recharge 1 000 fois reste sous quelques dizaines de Mo.

 5. pour chaque symbole S défini par la nouvelle unité :
        JitCompiler::Lookup( S, Addr )                  // force la matérialisation
        *reinterpret_cast<void**>( SlotAddrOf( S ) ) = Addr

    SlotAddrOf( S ) = JitCompiler::Lookup( "volt.fn." + S ) — résolu une fois et
    mis en cache dans JitRuntime.

    Ce store est la seule fenêtre de course. Il est atomique sur un pointeur aligné
    sur toutes les cibles visées, donc un thread concurrent lit soit l'ancienne
    adresse soit la nouvelle, jamais un mélange. C'est exactement la garantie que
    vm.md décrivait comme « la fenêtre de patch est entre deux instructions ».

 6. JitRuntime::RecordUnit( Unit.Ordinal, NewSig, Gen )
 7. return Ok{ PatchedSymbols = N }
```

Côté Driver, `volt run --watch` :

```
1. Compilation + Run initiaux dans un thread dédié.
2. Boucle de surveillance sur le thread principal : hash de contenu de chaque
   fichier du circuit (Core/Support/ContentHash.hpp, déjà utilisé par le cache
   frontend). Intervalle 200 ms — pas d'inotify en M3, la portabilité prime.
3. Sur changement du fichier F :
     a. Driver::RecompileUnit( F )    -> re-lex, parse, sema de CETTE unité
                                         contre le TypeStore gelé
     b. si diagnostics : les afficher, NE PAS recharger, garder le processus
     c. sinon : IJitBackend::Reload( NouvelleUnitView )
     d. rapporter Ok/Refused à l'utilisateur
```

`Driver::RecompileUnit` est nouveau mais n'introduit aucun mécanisme : `CompileUnit` possède déjà son propre
`StringInterner` et son propre `AstContext` (`Driver.hpp:41-81`), précisément pour que parse et sema d'un
fichier ne touchent aucun état mutable partagé. Recompiler une unité seule est ce que cette conception
rendait déjà possible.

### 4.5 Algorithme REPL

```
Session::Open()
 1. Driver construit normalement, stdlib chargée depuis le cache frontend.
 2. JitBackend avec { bPerUnitModules = true, bIndirectLinkage = false,
                      SkipUnitsBelow = StdlibUnitCount }.
    Pas d'indirection : une ligne de REPL n'est jamais rechargée, elle est
    remplacée par une ligne suivante qui redéfinit le nom. Le shadowing est
    déjà géré par le ScopeTable en amont.
 3. Begin + EmitUnit sur les unités pré-chargées (option -i), puis Finalize.
 4. JitEntryPoint::RunInitializers()  — exécute _V_init_* des unités chargées.

Session::EvalLine( Text )
 5. Nouvelle CompileUnit sur le même Driver, Ordinal = N (discovery order).
 6. Parse + sema de cette unité seule contre le TypeStore vivant.
    Si diagnostics : afficher, jeter l'unité, N inchangé. La session survit.
 7. Les bindings de haut niveau de la ligne deviennent des globals de module
    (StmtLocalDeclEmitter le fait déjà pour le top-level). MAIS leur STOCKAGE
    est alloué hors JIT :
        Addr = JitRuntime::AllocateBinding( Mangled, Size, Align )
               // Size/Align par LayoutEngine, jamais calculés ici
        JitCompiler::DefineAbsolute( { { Mangled, Addr } } )
    puis le module de la ligne les déclare externes.

    Pourquoi pas l'agrégat « session globals » ré-offsé de vm.md : cet agrégat
    grandit à chaque ligne, donc son stockage doit être réalloué, donc l'adresse
    de base bouge sous les lignes déjà compilées. Une allocation par binding dans
    une arène à chunks donne des adresses définitivement stables et supprime le
    problème. Les offsets restent calculés par LayoutEngine — c'est la même
    autorité ABI, appliquée à un binding au lieu d'un agrégat.

 8. Gen = OpenGeneration() ; AddModule( Gen, module de la ligne )
 9. JitEntryPoint::CallUnitInit( "_V_init_" + N )   // exécute les statements
10. Si la ligne est une expression en position de valeur, son initialiseur la
    laisse dans un binding synthétique que la session lit et affiche via le
    `to_s` résolu par la sema — jamais par une connaissance backend du type.
11. Si l'exécution laisse une exception en vol (tag != NoExceptionTag lu via
    __volt_unwind_slots), l'afficher, remettre le tag à NoExceptionTag, et
    continuer la session.

Session::Close()
12. Les générations sont libérées dans l'ordre inverse. L'arène est libérée en
    dernier — un binding peut être référencé par n'importe quelle génération.
```

Retrait de génération : `DropGeneration` existe et est sûr pour une ligne REPL qui n'a **défini aucune
fonction** — son code n'est plus atteignable une fois son initialiseur revenu. En M4, on ne le fait pas :
toutes les générations sont retenues jusqu'à `Close()`. C'est borné par la longueur de la session et évite
d'avoir à prouver qu'aucune closure créée par la ligne n'a survécu. L'affiner plus tard s'appuiera sur
`Raii::InferReturnOwnership` / `InferParameterEscape`, qui répondent déjà à cette question en amont.

---

## 5. Phase 3 : Stdlib pré-compilée

**Cette phase est à ~90 % déjà implémentée.** Il n'y a pas de nouveau pipeline à écrire.

Ce qui existe :

| Élément | Emplacement |
|---|---|
| Production du `.so` | `BackendLLVM/Private/Target/StdlibArtifact.cpp:101-157` (`BuildStdlibArtifact`) |
| Lien partagé | `LinkerDriver.cpp:170-211` (`LinkSharedLibrary`, `-shared -fPIC`) |
| Orchestration + cache | `DriverBuild.cpp:55-111` (`EnsureStdlibArtifact`) |
| Clé de cache | `Driver.cpp:281-293` (`ComputeNativeCacheKey` : frontend key × triple × opt × kind × lto) |
| Chemin | `~/.cache/volt/stdlib/<FrontendKey>/native/<NativeKey>.so` |
| Manifeste de symboles | `StdlibArtifact.cpp:68-92` (`WriteSymbolManifest`, `nm --defined-only -g`) |
| Drapeau CLI | `--stdlib-artifact shared` (`StdlibCache.cpp:3-25`) |
| Invariant d'ordinaux | `BackendInput::StdlibUnitCount` — la stdlib occupe toujours les ordinaux bas |

Ce que le JIT ajoute :

```
JitStdlibLoader::Locate( Driver, Options )
 1. EnsureStdlibArtifact( Driver, BuildOptions{ StdlibArtifactKind = "shared", ... } )
    -> réutilisé tel quel ; construit au premier appel, sert le cache ensuite.
 2. Si nullopt (--no-stdlib, ou échec) : StdlibPath vide, le prélude définit
    lui-même les globals partagés et __volt_unwind_slots vient du processus.

JitStdlibLoader::Attach( Compiler, Path )
 3. JitCompiler::AddDylib( Path )
      DynamicLibrarySearchGenerator::Load( Path.c_str(), /*GlobalPrefix=*/'\0' )
      GlobalPrefix nul : ELF ne préfixe pas les symboles (contrairement à Mach-O).
 4. Contrôle de sanité : lookup de "_V_init_0" et de UnwindTransport::SlotAccessorSymbol.
    Échec -> message nommant le .so et le symbole manquant, jamais un silence.
```

Le JIT **ne définit jamais** les unités `Ordinal < StdlibUnitCount` (§4.3 étape 5). C'est plus fort que ce que
fait l'AOT : lui les redéfinit partiellement (`bInlineEligibleOnly = true`, `LlvmLifecycle.cpp:76-81`) pour
permettre l'inlining. À O0, le JIT n'inline pas, donc il saute tout.

`_V_init_all` reste construit par le JIT (via `BackendCore::SynthesizeInitAll`, qui couvre **toutes** les
unités, stdlib comprise). Les `_V_init_<N>` de la stdlib vivent dans le `.so` et se résolvent par nom via le
générateur. L'ordre de lien du circuit est préservé parce que `SynthesizeInitAll` lit `BackendInput::Units`,
qui est déjà dans cet ordre.

`volt build-stdlib` n'est donc qu'un **réchauffement de cache** : il appelle `EnsureStdlibArtifact` avec
`kind = shared` et affiche le chemin produit. Utile en CI et au premier lancement, pas un pipeline.

---

## 6. Phase 4 : CLI & Build System

### 6.1 Les trois commandes

`cli-surface.md` fixe déjà les specs de `run` et `repl` ; on les respecte et on ajoute `--watch`.

```
Usage: volt run [options] [input_file] [-- ...]
    -i INPUT, --input INPUT          File input source program
    -s, --stdin                      Read input from stdin
    -w, --watch                      Recompile and hot-reload on file change
    -h, --help                       Show help

Usage: volt repl [options] [file]
    -i INPUT, --input INPUT          Pre-load a file into the REPL session
    -n, --no-history                 Disable history saving
    -h, --help                       Show help

Usage: volt build-stdlib [options]
    --kind KIND                      Artifact kind (static|shared, default shared)
    -O LEVEL                         Optimization level (0|1|2|3, default 2)
    --fresh                          Discard and rebuild the cached artifact
    -h, --help                       Show help
```

Chaque commande = 2 fichiers sous `Volt/{Public,Private}/Volt/CLI/Commands/` + une ligne
`TCommandRegister<T>` en fin de `.cpp`. **Zéro édition meson** : les sources sont globées et le registre est
un singleton auto-alimenté (`CommandRegistry.hpp:60-68`). `volt help` les liste automatiquement.

Les commandes réutilisent les groupes d'options existants : `GetInputOptions(InputFlags)`
(`CommandInputs.hpp:65-66`) et `StdlibCacheOptions(StdlibFlags)` (`StdlibCache.hpp:49`) — c'est l'idiome
meta-first déjà en place, aucune boucle argv ad hoc.

### 6.2 Le seam Driver

`Driver.hpp:200-220` est normatif : *« Driver est le seul endroit où `--target` se résout en `IBackend`
concret »*, et un fichier `Commands/` n'inclut jamais un header de backend. Le JIT suit :

```
source/Volt/Driver/Private/DriverRun.cpp        ★ neuf, sous #ifdef VOLT_ENABLE_JIT
```

```cpp
// Ajouts à Driver.hpp — données pures, aucun type Backend n'apparaît.
struct RunOptions
{
    bool bWatch          = false;
    std::uint8_t OptLevel = 0;
    std::string StdlibArtifactKind = "shared";
    bool bStdlibArtifactFresh      = false;
    bool bStdlibArtifactNoCache    = false;
    std::vector<std::string> ProgramArgs;
};

struct RunOutcome
{
    bool bOk          = false;
    std::int32_t Code = 0;
    std::string Message;
};

[[nodiscard]] DRIVER_EXPORT RunOutcome Run ( const RunOptions &Options );

// Session REPL : interface opaque, le backend reste invisible à l'appelant.
class DRIVER_EXPORT IReplSession
{
public:
    virtual ~IReplSession () = default;
    [[nodiscard]] virtual RunOutcome EvalLine ( std::string_view Text ) = 0;
    [[nodiscard]] virtual bool Ok () const = 0;
};

[[nodiscard]] DRIVER_EXPORT std::unique_ptr<IReplSession> OpenReplSession ( const RunOptions &Options );
```

`DriverRun.cpp` suit exactement la forme de `DriverBuild.cpp:173-195` : construire le backend, mapper
`MakeBackendViews()`, `Begin` → `EmitUnit*` → `Finalize`, puis `Run` au lieu de lire `EmitResult::Artifact`.

Sans `VOLT_ENABLE_JIT`, `Run` rend
`{ false, 0, "This build of volt was configured without the JIT (VOLT_ENABLE_JIT=OFF); volt run is unavailable" }`,
sur le modèle exact du garde LLVM de `DriverBuild.cpp:119-124`.

### 6.3 Build system

`meson.options` :

```meson
option('enable_jit', type : 'boolean', value : true,
       description : 'Build BackendJIT (LLVM ORC) and link it into Driver/Volt')
```

`source/Volt/meson.build` — l'ordre est porteur ; les trois modules LLVM passent **avant** `Driver` :

```meson
subdir('Core')
subdir('Frontend')
subdir('MiddleEnd')
subdir('Backend/BackendCore')
if get_option('enable_llvm')
    subdir('Backend/BackendLlvmIr')
    subdir('Backend/BackendLLVM')
    if get_option('enable_jit')
        subdir('Backend/BackendJIT')
    endif
endif
subdir('Driver')
subdir('Backend/BackendWASM')
subdir('Volt')
```

`enable_jit` est subordonné à `enable_llvm` : un `enable_jit=true` avec `enable_llvm=false` doit émettre un
`warning()` meson et se comporter comme `false`, pas échouer la configuration.

`meson/meson.build` — le bloc de découverte LLVM quitte `BackendLLVM/meson.build` (qui n'est plus le premier
consommateur) et rejoint le hub, qui héberge déjà `llvm_pch_header` :

```meson
if get_option('enable_llvm')
    llvm_dep = dependency('llvm', method: 'config-tool', static: false, required: true)
    llvm_include_dirs = include_directories(
        llvm_dep.get_variable(cmake: 'LLVM_INCLUDE_DIRS', default_value: '').split(';'),
        is_system: true,
    )
    llvm_rtti = llvm_dep.get_variable(cmake: 'LLVM_ENABLE_RTTI', default_value: 'ON')
    llvm_rtti_args = llvm_rtti in ['OFF', '0', 'NO'] ? ['-fno-rtti'] : []
endif
llvm_pch_header = meson.project_source_root() / 'source' / 'Volt' / 'Backend' / 'PchLLVM.hpp'
jit_pch_header  = meson.project_source_root() / 'source' / 'Volt' / 'Backend' / 'BackendJIT' / 'PchJIT.hpp'
```

**Aucune modification de dépendance n'est requise pour ORC.** `dependency('llvm', method: 'config-tool')` est
déclaré **sans `modules:`**, donc meson résout `llvm-config --libs` vers le `.so` monolithique `-lLLVM-22`.
Vérifié sur cette machine : LLVM **22.1.8**, et `nm -D` sur `libLLVM.so.22.1` exporte **2 232** symboles `orc`
et **58** symboles `LLJIT`. `llvm-config --components` liste `orcjit`, `orcshared`, `orctargetprocess`,
`jitlink`, `executionengine`. Les en-têtes sont dans le même paquet `-dev` déjà utilisé. Seule une
installation LLVM statique à composants séparés exigerait d'ajouter `modules: ['orcjit', 'jitlink', ...]`.

`BackendJIT/meson.build` suit le patron de `BackendVM/meson.build` (le plus propre : pas de bruit LLVM,
`unity=off`, `install: true`), avec en plus `llvm_dep`, `llvm_include_dirs`, `backendllvmir_dep` et son propre
PCH. `PchJIT.hpp` inclut `../PchLLVM.hpp` puis les en-têtes ORC — un PCH séparé, pour qu'ajouter ORC ne fasse
pas recompiler `BackendLlvmIr` et `BackendLLVM`.

`Driver/meson.build` :

```meson
if get_option('enable_llvm') and get_option('enable_jit')
    driver_deps += [backendjit_dep]
    driver_args += ['-DVOLT_ENABLE_JIT']
endif
...
if get_option('enable_llvm') and get_option('enable_jit')
    driver_link_deps += [backendjit_dep]
endif
```

---

## 7. Tests & Validation

L'existant : suite `golden` (diffs `volt parse`, 158 fixtures) et suite `samples`
(`tests/meson.build:145-182`), qui compile et exécute 96 fichiers `samples/Tests/**/*.vl` en comparant le
**code de sortie** à un `exit=N` dans un `.expected`.

### 7.1 Le levier gratuit

Une suite `jit` qui lance `volt run -i <file>` sur **exactement les mêmes fixtures** donne une parité
complète dès le premier jour, sans écrire une seule fixture :

```meson
test(name, sh, args: [
    '-c',
    'CODE=0; "$1" run -i "$2" >/dev/null 2>&1 || CODE=$?; ' +
    'EXP="$(grep "^exit=" "$3" | cut -d= -f2)"; [ "$CODE" -eq "$EXP" ]',
    'sh', volt_exe, sample_path, expected_path,
], suite: 'jit', workdir: meson.project_source_root())
```

### 7.2 Le test qui compte vraiment

Un test différentiel `volt run` ≡ `volt build && ./bin` sur chaque fixture. Il ne compare pas à une valeur
attendue mais **les deux backends l'un à l'autre** : c'est ce qui attrape une divergence d'émission entre la
queue AOT et la queue JIT, qui est la classe de bug que cette architecture rend possible.

### 7.3 Les tests spécifiques

| Test | Suite | Ce qu'il vérifie |
|---|---|---|
| `jit-tls-raise` | `jit` | **Le premier spike de M1.** Un `raise` traversant la frontière stdlib (`begin/rescue` en code JIT-é autour d'un appel stdlib qui lève). Valide `ETlsAccess::Accessor` de bout en bout. |
| `jit-stdlib-so` | `jit` | Le `.so` est produit, chargé, et `_V_init_0` + `__volt_unwind_slots` s'y résolvent. |
| `jit-differential` | `jit` | §7.2, sur les 96 fixtures. |
| `jit-reload-ok` | `jit` | Script : écrire F, lancer `--watch`, modifier le corps d'une fonction, vérifier le nouveau comportement sans redémarrage. |
| `jit-reload-refused` | `jit` | Modifier une **signature** → `Refused` avec un message nommant la fonction, processus vivant. |
| `jit-repl-smoke` | `jit` | Lignes sur stdin : binding, réutilisation à la ligne suivante, shadowing, erreur de sema qui ne tue pas la session. |
| `jit-repl-arena` | `jit` | Un binding créé ligne 1 reste lisible ligne 50 (adresses stables). |

Scripts sous `tests/jit/`, enregistrés depuis `tests/meson.build`.

### 7.4 Gate de non-régression

La Phase 1 ne peut être déclarée finie que si `golden`, `golden-lowered`, `golden-resolved` et `samples` sont
vertes **sans modification de fixture**. Aucun golden ne doit bouger : c'est un déplacement de fichiers.

À signaler sans le corriger ici : l'option `enable_testing` (`meson.options:6-7`) est déclarée mais jamais lue
par `tests/meson.build`. Écart préexistant, hors périmètre.

---

## 8. Roadmap

| Jalon | Contenu | Sortie mesurable | Estimation |
|---|---|---|---|
| **M0** | Extraction `BackendLlvmIr`. §3 intégral, plus les deux changements de forme et les deux nettoyages. | `volt build` inchangé, 4 suites vertes | ~1 semaine |
| **M1** | **MVP `volt run`.** Granularité `Whole`, `ETlsAccess::Accessor`, stdlib `.so`, `LLJIT`, liaison directe, pas de reload. Spike TLS **en premier**. | suites `jit` + `jit-differential` vertes | ~1 semaine |
| **M2** | Granularité `PerUnit` + module prélude. `bDefineGlobals`, reset du cache `FunctionRegistry`. | `jit-differential` reste verte en `PerUnit` | ~4 jours |
| **M3** | `volt run --watch`. `ELinkage::Indirect`, `@volt.fn.*`, `DiffRefusal`, `Driver::RecompileUnit`, boucle de surveillance. | `jit-reload-ok` + `jit-reload-refused` | ~4 jours |
| **M4** | `volt repl`. Arène de session, `absoluteSymbols`, générations par ligne, boucle interactive. | `jit-repl-smoke` + `jit-repl-arena` | ~1 semaine |
| **M5** | Compilation paresseuse/étagée : `LLLazyJIT`, `CompileOnDemandLayer`, re-JIT à O2 des fonctions chaudes. | mesure de démarrage sur un gros circuit | ultérieur |
| **M6** | *Codegen propre* — remplacer la queue LLVM du JIT par un émetteur machine direct. **Non planifié.** Ne se justifie que si le temps de compilation ORC devient dominant, ce que M5 mesurera. | — | non planifié |

**MVP = M0 + M1 ≈ 2 semaines** pour un développeur expérimenté, ce qui correspond à la cible.

Ordre de valeur décroissante : M1 supprime le coût principal (optimisation + `.o` + lien). M3 et M4 sont du
confort de boucle. M5 est une optimisation d'une optimisation.

---

## 9. Risques & Mitigations

| # | Risque | Gravité | Mitigation |
|---|---|---|---|
| 1 | **TLS dans le code JIT-é.** Relocations thread-local non résolues sans `ELFNixPlatform` + `liborc_rt`, et le `liborc_rt` disponible ici est en compiler-rt **21.1.8** contre LLVM **22.1.8**. | Bloquante | Neutralisée par conception : `ETlsAccess::Accessor` (§4.2.1). Ne dépend d'aucun runtime ORC. **`jit-tls-raise` est le premier test écrit en M1**, avant tout le reste. |
| 2 | **Régression en Phase 1.** 55 fichiers déplacés entre modules. | Élevée | Déplacement pur, aucune signature d'émission modifiée. Commit isolé, sans code JIT. Gate : 4 suites vertes, zéro golden modifié. |
| 3 | **Modules par unité : globals dupliqués.** Trois slots d'unwind, stockage, préordre, table de symboles, entrée. | Moyenne | Module prélude unique + `bDefineGlobals`. Et avec la stdlib `.so` attachée, c'est elle qui les définit — le cas ne se pose qu'en `--no-stdlib`. |
| 4 | **Mémoire non bornée en `--watch`.** Les anciennes générations ne sont jamais démappées. | Moyenne | Choix délibéré : correction avant empreinte. ~quelques dizaines de Ko par rechargement ; 1 000 rechargements restent sous quelques dizaines de Mo. Documenté dans `jit.md`. Un démappage sûr exigerait de connaître les frames vivantes, ce que M3 ne peut pas faire. |
| 5 | **`volt run` indisponible sans LLVM.** `vm.md` promettait « zéro dépendance externe — la VM est embarquée dans le compilateur ». | Moyenne | Perte réelle et assumée en retirant `vm.md`. `jit.md` doit l'écrire noir sur blanc, et `DriverRun.cpp` doit rendre un message explicite, pas un crash. |
| 6 | **Visibilité des symboles du `.so` de stdlib.** Un symbole caché est invisible au générateur de recherche. | Faible | `WriteSymbolManifest` produit déjà le `.meta` via `nm --defined-only -g` : le contrôle existe. `JitStdlibLoader::Attach` teste `_V_init_0` et `__volt_unwind_slots` au chargement et échoue en nommant le symbole manquant. |
| 7 | **Divergence AOT/JIT.** Deux queues sur un émetteur commun peuvent diverger sur ce que la queue ajoute (optimisation, entrée). | Faible | `jit-differential` (§7.2) est écrit exactement pour ça et tourne sur les 96 fixtures. |
| 8 | **`PointerSize = 8` codé en dur** dans `LayoutEngine.cpp`, alors que `abi.md` promet un paramètre de constructeur pour wasm32. | Nulle ici | Sans effet : le JIT est in-process, donc l'hôte est la cible. À noter pour `BackendWASM`, hors périmètre. |
| 9 | **Poids de maintenance**, le point juste de `AUDIT.md` §13. | Structurelle | Un seul émetteur sémantique (~6 900 LOC partagés), deux queues courtes (~1 250 et ~900 LOC). Le total *net* ajouté par le JIT est la queue ORC, pas un second backend. |

---

## 10. Checklist des fichiers

### Nouveaux — `BackendJIT/`

```
source/Volt/Backend/BackendJIT/
├── meson.build
├── PchJIT.hpp                          (= ../PchLLVM.hpp + en-têtes ORC)
├── Private/
│   ├── JitBackend.cpp
│   ├── JitCompiler.hpp / .cpp          (LLJIT, générations, lookup, dylibs)
│   ├── JitModuleBuilder.hpp / .cpp     (pilote Ir::IrGenerator en PerUnit)
│   ├── JitSymbolResolver.hpp / .cpp    (générateurs de symboles)
│   ├── JitRuntime.hpp / .cpp           (indirection, signatures, arène)
│   ├── JitEntryPoint.hpp / .cpp        (résolution + appel de __volt_entry)
│   └── JitStdlibLoader.hpp / .cpp      (localise + attache le .so)
└── Public/Volt/BackendJIT/
    └── JitBackend.hpp                  (pimpl, zéro header LLVM)
```

### Nouveaux — `BackendLlvmIr/`

```
source/Volt/Backend/BackendLlvmIr/
├── meson.build
├── Public/Volt/BackendLlvmIr/
│   ├── IrGenerator.hpp                 (pimpl, zéro header LLVM)
│   └── LlvmAccess.hpp                  (consommateurs LLVM-aware uniquement)
└── Private/                            (≈ 55 fichiers déplacés, cf. §3.2)
    ├── Core/{ModuleContext.*, EmitterServices.hpp, LlvmFwd.hpp}
    ├── Functions/**                    (14 fichiers)
    ├── Lower/**                        (36 fichiers)
    ├── Types/{TypeMapper.*, AbiVerifier.*}
    └── IrGenerator.cpp                 ★ neuf : orchestration + granularité
```

### Nouveaux — `BackendCore/`

```
Public/Volt/BackendCore/ExecutableBackend.hpp     ★ IJitBackend, RunResult, ReloadResult
Public/Volt/BackendCore/AbiClassifier.hpp         ★
Public/Volt/BackendCore/InstructionSchema.hpp     ★
Public/Volt/BackendCore/Instructions.inl          ★ manifeste, opcodes neutres
Public/Volt/BackendCore/DiagnosticSink.hpp        ★ déplacé
Private/DiagnosticSink.cpp                        ★ déplacé
Private/AbiClassifier.cpp                         ★
Private/InstructionSchema.cpp                     ★
Private/MonoQueue.cpp                             ★ (MonoDriver.cpp + MonoLookup.cpp)
Private/LayoutOfValue.cpp                         ★ déplacé
```

### Modifiés

| Fichier | Modification |
|---|---|
| `meson.options` | + `enable_jit` |
| `meson/meson.build` | bloc `llvm_dep` remonté ; `llvm_pch_header` repointé ; + `jit_pch_header` |
| `source/Volt/meson.build` | + `BackendLlvmIr`, `BackendJIT` avant `Driver` ; − `BackendVM` |
| `source/Volt/Driver/meson.build` | + `backendjit_dep`, `-DVOLT_ENABLE_JIT` |
| `source/Volt/Backend/BackendLLVM/meson.build` | bloc `llvm_dep` retiré ; + `backendllvmir_dep` |
| `source/Volt/Driver/Public/Volt/Driver/Driver.hpp` | + `RunOptions`, `RunOutcome`, `Run`, `IReplSession`, `OpenReplSession`, `RecompileUnit` |
| `BackendCore/Public/.../UnwindTransport.hpp` | + `SlotAccessorSymbol` + disposition du bloc |
| `BackendCore/Public/.../Monomorphizer.hpp` | + `MonoQueue::Drain`, `MonoLookup` |
| `BackendLlvmIr/.../ModuleContext.hpp` | `LLVMContext` par `unique_ptr` ; `TargetSpec` ; `TargetMachine` optionnel |
| `BackendLlvmIr/.../ExceptionLowering.hpp` | + `ETlsAccess`, `bDefineGlobals` |
| `BackendLlvmIr/.../ExceptionGlobals.cpp` | branchement `Direct` / `Accessor` |
| `BackendLlvmIr/.../ExprResolvedCallEmitter.cpp` | branchement `ELinkage` `Direct` / `Indirect` |
| `BackendLlvmIr/.../FunctionRegistry.cpp` | reset du cache par module en `PerUnit` |
| `BackendLlvmIr/.../EntryPointEmitter.cpp` | mode « pas de shim CRT » (`EntrySymbol` vide) |
| `BackendLlvmIr/.../VTableRegistry.hpp` | nettoyage : `LlvmFwd.hpp` au lieu de `<llvm/IR/GlobalVariable.h>` |
| `BackendLlvmIr/.../ExprEmitter.cpp` | nettoyage : bras `DynamicUpcast` extrait en fonction nommée |
| `tests/meson.build` | + suite `jit` (7 tests, §7.3) |

### Nouveaux — CLI et Driver

```
source/Volt/Volt/Public/Volt/CLI/Commands/RunCommand.hpp
source/Volt/Volt/Private/Volt/CLI/Commands/RunCommand.cpp
source/Volt/Volt/Public/Volt/CLI/Commands/ReplCommand.hpp
source/Volt/Volt/Private/Volt/CLI/Commands/ReplCommand.cpp
source/Volt/Volt/Public/Volt/CLI/Commands/BuildStdlibCommand.hpp
source/Volt/Volt/Private/Volt/CLI/Commands/BuildStdlibCommand.cpp
source/Volt/Driver/Private/DriverRun.cpp
tests/jit/*.bash                                  (5 scripts, §7.3)
```

### Supprimés

```
source/Volt/Backend/BackendVM/**                  (6 fichiers)
  meson.build, Private/VmBackend.cpp,
  Public/Volt/BackendVM/{Bytecode.hpp, Bytecode.inl, VirtualMachine.hpp, VmBackend.hpp}
.agents/backend/vm.md
```

### Documentation

| Fichier | Modification |
|---|---|
| `.agents/backend/jit.md` | **neuf** — spec détaillée, remplace `vm.md`, en miroir de `llvm.md` |
| `.agents/backend/vm.md` | **supprimé** |
| `.agents/BACKEND.md` | diagramme des cibles : #1 → `BackendJIT` ; mention de `BackendLlvmIr` |
| `.agents/backend/core-interfaces.md` | + `IJitBackend`, `AbiClassifier`, `Instructions.inl` partagé |
| `.agents/backend/llvm.md` | note d'en-tête : l'émission vit désormais dans `BackendLlvmIr` |
| `.agents/rules/cli-surface.md` | + `run --watch`, + `build-stdlib`, `run`/`repl` → `BackendJIT` |
| `.agents/agents/volt-build-nix.md` | + option `enable_jit`, + cible `build-stdlib` |

---

## Annexe — Vérification du plan

- Les 82 fichiers de `BackendLLVM/Private/` apparaissent chacun exactement une fois en §3.2.
- Chaque appel ORC cité en §4.2 a été vérifié présent dans les en-têtes LLVM 22.1.8 installés
  (`Orc/LLJIT.h`, `Orc/Core.h`, `Orc/ExecutionUtils.h`, `Orc/ThreadSafeModule.h`, `Orc/AbsoluteSymbols.h`).
- Aucune quatrième annotation n'est proposée — la liste fermée `@[Primitive]` / `@[External]` / `@[Literal]`
  de `rules/zero-hardcode.md` est intacte.
- Le seul symbole runtime introduit, `__volt_unwind_slots`, est nommé dans `BackendCore` et concerne de la
  machinerie que `UnwindTransport.hpp` spécifie déjà — pas une construction de niveau Volt
  (`rules/backend-machine-only.md`).
- `Instructions.inl` reste un manifeste ; aucun `switch` sur opérateur n'est introduit
  (`rules/meta-first.md`).
- `BackendJIT/Public/` ne contient aucun header LLVM ; `PchJIT.hpp` est sous `Private`-équivalent (racine du
  module, comme `PchLLVM.hpp` aujourd'hui).
