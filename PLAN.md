# Issue : Add Circuits — Plan d'attaque par phases

## Contexte

Un **circuit** (`Project.vl` à la racine d'un projet) est le manifest Volt : nom d'app, version runtime, entrypoint, et mapping `"Module" => "chemin/dir"`. Il permet à l'interpréteur de résoudre les dépendances multi-fichiers **à l'avance** (pas de découverte récursive à la Crystal), via les annotations `@[Link("Module")]` dans les fichiers `.vl`. Objectif final : `volt run` exécute les projets `Samples/Tests/Circuits/TwoDeps` et `DiamandDeps` de bout en bout, et `volt circuit` auto-génère/synchronise `Project.vl`. Les `battery` sont **hors scope** (v1.0).

## État des lieux (exploration faite)

- **Lexer** (`Source/Volt/Frontend/Lexer/`) : pas de token `=>` (seulement `Spaceship <=>`), pas de keyword `circuit`. Table `KEYWORDS` dans `Lexer.cr:9`, `TokenKind` dans `Token.cr`. `Span` porte déjà `file` → diagnostics multi-fichiers OK.
- **Parser** (`Source/Volt/Frontend/Parser/InternalParser/ParseTopLevel.cr`) : dispatch top-level `parse_top_level_node` (l.31), `collect_annotations` (l.66) attache les annots aux decls — **pas d'annotation "libre" niveau fichier** (un `@[Link]` seul avant `module` casse). `parse_use_decl` existe (l.231, chemins `A::B`).
- **AST** (`Source/Volt/Frontend/AST/Decl.cr`) : `Annotation`, `ModuleDecl`, `UseDecl`, `ExternDecl`… Pas de HashLiteral ni CircuitDecl. `Program` = `nodes + file` (un seul fichier).
- **Sémantique** : `Frontend.analyse(source, file)` **et** surcharge `Frontend.analyse(program : Program)` (`Frontend/__all__.cr:19,24`) → point d'entrée idéal pour un Program fusionné multi-fichiers. `TypeCollector` résout déjà modules/classes/structs (`resolve_module`, etc.).
- **Samples circuits** : `Samples/Tests/Circuits/{TwoDeps,DiamandDeps}` déjà créés par l'utilisateur (fixtures de référence). ⚠️ `TwoDeps/Source/Components/Logger.vl` a un `end` manquant (class non fermée) — à corriger.
- `grep Link` : aucune gestion de `@[Link]` Volt dans les sources (seuls des `@[Link]` Crystal FFI).

## Décisions de design (hypothèses à valider à la review)

1. **Syntaxe du bloc** : les deux fixtures utilisent `circuit "Name" { ... }` mais l'issue montre `circuit "Name" do ... end`. → Le parser acceptera **les deux** (`{`…`}` et `do`…`end`), coût marginal nul ; les fixtures restent en `{}`.
2. **Granularité du Link** : `@[Link("Models")]` charge **tous les `.vl` du répertoire du module** (récursif), lazy — seuls les modules réellement atteints depuis l'entrypoint sont chargés.
3. **Découverte du manifest** : `volt run <file>` remonte les répertoires depuis le fichier pour trouver `Project.vl` ; `volt run` sans argument = `Project.vl` du cwd → son `entrypoint`. Sans `Project.vl`, comportement mono-fichier actuel inchangé (zéro régression).
4. **Project.vl est du Volt** : parsé par le lexer/parser standards (nouveau nœud `CircuitDecl`), jamais compilé/exécuté par la VM — consommé au chargement.
5. `runtime "0.1.0"` / `entrypoint "..."` : parsés comme entrées du circuit (appels sans parenthèses à l'intérieur du bloc circuit uniquement).

---

## Phase 1 — Syntaxe : token `=>`, HashLiteral, bloc `circuit` | DONE WITH `Chore/Add-Circuits/Phase1/Hash`

**But** : `./bin/Volt parse Project.vl` dumpe un AST correct.

- `Frontend/Lexer/Token.cr` : ajouter `TokenKind::FatArrow` (`=>`) + keyword `Circuit` ; `Lexer.cr` : scanner `=>` (attention à l'ordre avec `=` / `==`), entrée `"circuit"` dans `KEYWORDS`.
- `Frontend/AST/Expr.cr` : nouveau `HashLiteralExpr` (paires `Array({AExpr, AExpr})`) — générique, réutilisable au-delà des circuits (demande explicite de l'issue : "créer un Hash pour résoudre les `=>`").
- `Frontend/AST/Decl.cr` : nouveau `CircuitDecl < ADecl` (name, runtime, entrypoint, modules : HashLiteralExpr, loc) — champ réservé batteries plus tard.
- `Frontend/Parser/InternalParser/ParseTopLevel.cr` : brancher `when .circuit?` → `parse_circuit_decl` (nouveau fichier `ParseCircuit.cr` pour rester modulaire) : `circuit <String> ({ | do) entries (} | end)`. Entrées : `runtime <String>`, `entrypoint <String>`, `modules( <hash pairs> )` avec virgule terminale tolérée (cf. fixtures).
- `ParseLiteral.cr` ou `ParseCircuit.cr` : parsing des paires `expr => expr`.
- `Frontend/AST/Dump.cr` : dump des nouveaux nœuds.
- **Specs** : `Spec/Frontend/Lexer` (token `=>`), `Spec/Frontend/` parser (circuit `{}`, `do/end`, hash trailing comma, erreurs : clé non-string, entrée inconnue).

## Phase 2 — Modèle : `Circuit::Manifest` (validation sémantique) | DONE WITH `Chore/Add-Circuits/Phase2/Manifest`

**But** : transformer un `CircuitDecl` en manifest typé et validé, avec diagnostics `file:line:col`.

- Nouveau répertoire `Source/Volt/Circuit/` :
  - `Manifest.cr` — struct typée : `name`, `runtime : String`, `entrypoint : String`, `modules : Hash(String, String)`.
  - `Loader.cr` — `Circuit.load(path) : Manifest` : lit `Project.vl`, parse, valide (exactement un `circuit`, entrypoint existe sur disque, chemins de modules existants et **confinés à la racine du projet** (pas de `../` path traversal), pas de module dupliqué). Erreurs via le système `Diagnostic` existant (`Frontend/Diagnostic/`).
- **Specs** : `Spec/Circuit/` — manifests valides (fixtures TwoDeps/DiamandDeps), invalides (module dupliqué, chemin inexistant, entrypoint manquant, deux blocs circuit, path traversal).

S'assurer de restreindre les chemins des modules définis dans le manifest pour qu'ils soient résolus relativement à la racine du projet et valider qu'ils ne sortent pas du projet via des `../` malveillants.

## Phase 3 — `@[Link]` + résolveur de dépendances multi-fichiers | TODO with `Chore/Add-Circuits/Phase3/Resolver`

**But** : depuis l'entrypoint, charger le graphe de modules et produire un `Program` fusionné analysable.

- **Annotation niveau fichier** : `ParseTopLevel.cr` — quand `collect_annotations` rencontre `@[Link("X")]` non suivi d'une decl compatible, produire un nœud `LinkDecl < ADecl` (nouveau, dans `Decl.cr`) au lieu d'échouer. Validation : 1 arg string.
- `Source/Volt/Circuit/Resolver.cr` :
  - Depuis l'entrypoint : parser, collecter les `LinkDecl`, mapper via `manifest.modules`, charger tous les `.vl` du répertoire du module, récursivement.
  - **Diamant** : set `visited` par module (chargé une seule fois — fixture DiamandDeps).
  - **Cycles** : détection (stack DFS) → diagnostic clair (pas de stack overflow).
  - **Ordre** : tri topologique, dépendances d'abord, entrypoint en dernier.
  - Sortie : `Program` fusionné (nodes concaténés dans l'ordre topo ; `Span.file` garde la provenance) + `Hash(filename => source)` pour le renderer.
- Brancher `volt check` sur ce chemin (validation sans exécution).
- **Specs** : Resolver sur les deux fixtures (ordre topo, dédup diamant), cycle artificiel (fixture `CycleDeps` à créer), `@[Link]` vers module non déclaré → diagnostic.

## Phase 4 — Exécution : `volt run` project-aware (bout en bout) | TODO with `Chore/Add-Circuits/Phase4/Run`

**But** : les circuits **fonctionnent** — les deux fixtures s'exécutent.

- `CLI/Command/Run.cr` : découverte de `Project.vl` (remontée depuis le fichier ; cwd si pas d'argument → entrypoint du manifest). Si trouvé : pipeline `Resolver → Frontend.analyse(program) → BytecodeCompiler → passes → Vm` (identique à `interpret`, avec la surcharge `analyse(Program)`), sinon chemin mono-fichier inchangé. `DiagnosticRenderer` reçoit la map multi-fichiers complète.
- Corriger `Logger.vl` (le `end` manquant) + ajuster `Main.vl` des fixtures pour produire une sortie vérifiable (stdout/exit code).
- **Specs fonctionnelles** : suivre le pattern `Spec/Samples/01.cr` + `Data.cr` — `RunVolt` sur les deux projets avec stdout/exit attendus. ⚠️ Risque : dépend du support runtime classes/structs/modules (présent d'après `Vm.cr` class tables / `TypeCollector`, mais si un trou apparaît, la spec de repli valide `volt check` exit 0 + un couple de fichiers minimal exécutable, et le trou est documenté.

## Phase 5 — `volt circuit` : auto-génération / sync de `Project.vl` | TODO with `Chore/Add-Circuits/Phase5/Circuit`

**But** : la commande stub devient réelle.

- `CLI/Command/Circuit.cr` : remplacer le stub (supprimer le `sleep 0.1`) :
  - **Création** : scan des répertoires de premier niveau sous `Source/` contenant des `.vl` → entrées `modules`, détection entrypoint (`Source/Main.vl`), `runtime` = version courante.
  - **Update (préservation)** : si `Project.vl` existe → `Circuit.load`, merge (nouveaux dossiers ajoutés, mappings custom et futurs `battery` préservés, modules disparus signalés), réécriture formatée.
  - **Idempotence** : deux runs consécutifs = zéro diff.
- **Specs** : génération dans dossier temp, update préservant un mapping custom, idempotence.

---

## Fichiers touchés (récap)

| Action | Fichiers |
|---|---|
| Modif | `Lexer/Token.cr`, `Lexer/Lexer.cr`, `AST/{Expr,Decl,Dump}.cr`, `Parser/InternalParser/ParseTopLevel.cr`, `CLI/Command/{Run,Circuit}.cr`, fixtures `Samples/Tests/Circuits/**` |
| Créés | `Parser/InternalParser/ParseCircuit.cr`, `Source/Volt/Circuit/{Manifest,Loader,Resolver}.cr`, `Spec/Circuit/*`, `Spec/Frontend/*` (nouveaux cas), fixture `CycleDeps`, entrées `EXPECTED_DATA` |

## Vérification

1. `krystal -s` — toute la suite (nouvelles specs incluses) verte à chaque phase.
2. `krystal -x` puis :
   - `./bin/Volt parse Samples/Tests/Circuits/TwoDeps/Project.vl` (Phase 1)
   - `./bin/Volt check` / `run` sur `TwoDeps` et `DiamandDeps` (Phases 3–4)
   - `./bin/Volt circuit -d <tmp>` deux fois → idempotent (Phase 5)
3. Non-régression : les samples fonctionnels existants (`Samples/Tests/Functional/`) passent inchangés.
4. Boucle qualité do-issue : agents reviewers (sécurité/qualité/tests/archi) en fin d'implémentation, corrections jusqu'à 4× PASS.

## Points à confirmer (review)

- `{}` **et** `do/end` acceptés pour le bloc circuit
  > **explications:** en Crystal|Ruby|Volt, un bloc `do ... end` est équivalent à `{ ... }`.

- `@[Link]` charge le répertoire entier du module (pas fichier par fichier)
  > **explications:** Volt ne fait pas de découverte récursive à la Crystal, mais un module est généralemnet un répertoire contenant des `.vl`. Le manifest `Project.vl` mappe `"Module" => "chemin/dir"`.
  > **⚠️ Attention :** un module keyword `module` n'est PAS forcément le nom du répertoire. C'est le nom du dossier qui est mappé dans le manifest. Cela reste une norme de good practice de nommer le module Volt comme le nom du dossier, ça aide également le linkage.
