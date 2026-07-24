# Backend Architecture — Declarative Code Generation & Targets

## Rôle du Backend
Le Backend de Volt prend en entrée l'AST/HIR **entièrement désucré, résolu et typé** issu du MiddleEnd.

Sa philosophie fondamentale est la **génération déclarative par Pattern Matching** :
- **Zéro analyse sémantique dans le Backend.**
- **Zéro inférence ou résolution de type dans le Backend.**
- **Zéro incertitude.**

Le Backend se contente de parcourir les nœuds AST/HIR déjà annotés et d'émettre le code cible correspondant selon la cible choisie.

> **Contrat d'entrée : [`rules/core-ast.md`](rules/core-ast.md).** Le backend
> reçoit exactement **27 nœuds noyau** (les 9 sucre ont disparu, vérifié par
> `AstInvariant`), chacun typé — d'emblée en code concret, après substitution
> dans un corps générique. `CalleeResolution` (méthode vs instruction machine
> sur `Primitive{ Spelling, Bits }`) et `ClosureEnvFrame` (`bEscapes`, taille,
> alignement) sont déjà calculés. Ce fichier dit aussi ce qui est **refusé
> bruyamment** (`T?`, `Array#to_string`, runtime JSX) plutôt que laissé au
> backend, et où sont les trous stdlib restants.

---

## Les 3 Cibles Backend de Volt

```
                     ┌───> [1] volt run   ───> Interpréteur / JIT ABI (Dev & Hot Reload)
MiddleEnd (AST/HIR) ─┼───> [2] volt JSX   ───> WebAssembly / WASM (Frontend Web)
                     └───> [3] volt build ───> LLVM IR / Native AOT (Prod & Backend)
```

### 1. Target `run` — Interpréteur / JIT ABI (`volt run`)
- **Cas d'usage** : Boucle de développement rapide, exécution instantanée sans phase de compilation lourde, et **Hot Reloading** pour le développement d'applications Web et d'interfaces.
- **Fonctionnement** : Évalue directement l'AST/HIR résolu ou génère du bytecode VM léger exécutable immédiatement en mémoire.

### 2. Target `JSX` — WebAssembly (`volt build --target wasm`)
- **Cas d'usage** : Applications Web modernes, composants UI dynamiques et interactifs dans le navigateur.
- **Fonctionnement** : Traduit les nœuds UI/JSX abaissés et la logique métier en bytecode WebAssembly (WASM) compact et ultra-performant pour le Web.

### 3. Target `build` — LLVM IR / Executable Native (`volt build`)
- **Cas d'usage** : Applications de production, serveurs backend hautes performances, et exécutables natifs optimisés.
- **Fonctionnement** : Génère le code intermédiaire LLVM IR à partir de l'AST/HIR résolu, puis s'appuie sur le pipeline d'optimisation et d'émission native de LLVM.

---

## Pipeline d'émission par Pattern Matching

Grâce à la séparation stricte des rôles, la structure d'émission du Backend s'appuie sur le pattern matching :

```cpp
// Structure conceptuelle du générateur Backend :
void EmitNode( const ResolvedNode &Node )
{
    std::visit( Overloaded {
        [&]( const ResolvedLambda &Lambda )   { EmitLambdaTarget( Lambda ); },
        [&]( const ResolvedCall &Call )       { EmitCallTarget( Call ); },
        [&]( const ResolvedBinary &Binary )   { EmitBinaryTarget( Binary ); },
        [&]( const ResolvedLiteral &Lit )     { EmitLiteralTarget( Lit ); },
        // ...
    }, Node );
}
```

Le Backend n'a pas à se soucier de savoir si un entier `10` est `Int32` ou `UInt64`, ni si un appel est valide : le MiddleEnd lui fournit l'information exacte et incontestable.
