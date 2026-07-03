# Specification Technique : Le Système Objet de Volt

Ce document présente l'architecture, la sémantique etle modèle d'exécution de la programmation orientée objet dans le langage Volt.
<br>
Volt exclue l'usage d'un Garbage Collector afin d'éliminer les pauses de latence et les barrières d'écriture au runtime. La gestion de mémoire s'appuie entièrement sur l'analyse statique des durées de vie à la compilation et sur l'insertion de primitives de destruction déterministes RAII.

---

## 1. Structs vs. Classes ( Valeur vs. Référence )

Les types `struct` et `class` dictent l'organisation physique des données et leur transit au sein de l'interpréteur.

```txt
        [ Cadre de Pile VM ]                   [ Tas (Heap) ]
    ┌────────────────────────┐             ┌───────────────────┐
    │ R1: [ Ptr Struct ] ────┼──┐          │ 0x00: Header      │
    ├────────────────────────┤  │          │       (TypeID...) │
    │ Spilled Struct Data    │◄─┘          ├───────────────────┤
    │ (Flat raw words)       │             │ 0x08: Field 1     │
    └────────────────────────┘             │ 0x10: Field 2     │
                                           └───────────────────┘
```

### Struct: Allocation sur la stack, sémantique de valeur

Les structures sont des types valeurs purs. Elles ne possèdent aucune métadonnée d'objet (pas de `TypeID` ni d'entête).

*   **Organisation en Mémoire :** Allouées de manière contiguë dans le cadre de pile de la VM (Stack-Allocation).
*   **Transit des Registres :** Seul un pointeur léger vers la base de cette allocation transite dans un registre de la VM. Cette indirection locale évite de saturer l'allocateur de registres.
*   **Sûreté contre l'échappement :** L'échappement de pointeurs de pile est interdit. Tout mouvement d'un struct (assignation, retour de fonction, ou stockage dans une variable d'instance d'une classe) déclenche une copie physique par valeur du bloc mémoire complet via l'opcode `COPY_BLOCK`.
*   **Dispatch :** Les méthodes d'un `struct` sont résolues statiquement à la compilation. L'appel est converti en une instruction `CALL` directe vers un index de fonction constant.

### Class: Allocation sur le tas, sémantique de référence

Les classes sont des types de référence alloués dynamiquement sur le tas.

*   **Allocation :** Allouées via l'opcode `INIT_OBJ` qui applique une zéro-initialisation de la mémoire.
*   **Entête d'Objet (Header) :** Chaque instance porte un entête de 64 bits contenant un `TypeID` de 32 bits et des indicateurs de statut (flags de cycle de vie et taille). Le `TypeID` sert d'index dans la table globale des métadonnées du runtime pour retrouver la VTable et l'ITable de la classe.
*   **Dispatch :** Les appels de méthodes d'instances utilisent un dispatch dynamique basé sur une VTable primaire linéaire (`CALL_METHOD`) où les index des méthodes sont résolus statiquement à la compilation.

---

## 2. Modules vs. Mixins (Espaces de noms vs. Traits injectables)


Volt distingue strictement l'organisation des symboles statiques du partage de code d'instance.

### Module (`module`) : Espace de noms statique
Un `module` est un conteneur d'outils, de constantes, de fonctions statiques et de définitions de types.

*   **Contraintes physiques :** Un module ne peut pas être instancié, ne possède ni constructeur ni destructeur, et ne participe pas aux VTables ou ITables.
*   **Pas de contexte d'instance :** Ses méthodes n'ont pas accès à un pointeur `self`.
*   **Résolution :** Le nom du module sert de préfixe lors de la résolution de l'identifiant. L'appel d'une méthode de module génère un opcode `CALL` direct vers un chunk ordinaire (complexité d'appel *O(1)*, aucun surcoût VM).
*   **Contrôle sémantique :** Tenter d'instancier un module ou d'écrire `include MonModule` lève un diagnostic d'erreur de compilation.

### Mixin (`mixin`) : Trait d'instance injectable
Un `mixin` est un ensemble de méthodes d'instance destiné à être injecté dans une ou plusieurs classes via le mot-clé `include`.

*   **Dispatch :** Les appels de méthodes de mixin s'effectuent via l'opcode `CALL_MIXIN`.
*   **Interface Tables (ITables) :** Pour résoudre le problème des offsets de méthode divergents induit par l'héritage multiple, chaque classe implémentant un mixin possède une ITable. Au niveau de l'interpréteur Tier-0, l'ITable est structurée comme un tableau contigu de paires `{ ModuleID, VTablePtr }`. La VM effectue un parcours linéaire pour retrouver la VTable spécifique du mixin et appeler la méthode.
*   **Accès à l'état :** Un mixin peut manipuler les variables d'instance de la classe hôte (ex. `@price`). L'analyseur sémantique s'assure lors de la compilation que le mixin n'est inclus que dans des classes possédant les variables d'instance requises au bon offset statique.

---

## 3. Classes Abstraites & Hiérarchie d'Héritage

L'héritage simple de classes s'accompagne de l'héritage multiple de traits (via les mixins).

*   **Règle de Nommage :** Toutes les classes abstraites sont préfixées par la lettre `A` (ex. `AProduct`, `ADevice`).
*   **Héritage d'état (Champs Plats) :** Le layout d'une sous-classe commence obligatoirement par le layout complet de sa classe parente. Cette préfixation partagée garantit qu'un champ hérité se trouve exactement au même offset mémoire, que la VM lise l'instance parent ou l'instance enfant. Les accès s'effectuent sans recherche dynamique.
*   **Méthodes Abstraites :** Une classe abstraite définit des signatures de méthodes sans implémentation. Le compilateur s'assure qu'une classe concrète implémente la totalité des méthodes abstraites de ses parents. Tenter d'instancier une classe abstraite génère une erreur de compilation immédiate.


---

## 4. Cycle de vie des Objets & Sûreté de l'Unwinding

La garantie de non-fuite mémoire s'appuie sur le compilateur pour automatiser l'insertion des drop-points sans intervention d'un Garbage Collector.

### A. La routine `__drop_fields` (Deep Drop)
Pour éviter de laisser des objets orphelins sur le tas lors de la libération d'un objet parent, le compilateur génère une méthode interne **`__drop_fields`** pour chaque classe. Cette routine parcourt les champs de type référence de l'objet et émet une instruction `DROP` sur chacun d'eux (les pointeurs nuls étant ignorés). 

Le processus de destruction d'une instance s'exécute dans l'ordre suivant :

```txt
[ Appel de DROP sur l'objet ]
             │
             ▼
┌────────────────────────────┐
│  Exécution de finalize     │  ◄─── (Destructeur utilisateur, si présent)
└────────────┬───────────────┘
             │
             ▼
┌────────────────────────────┐
│ Appel de __drop_fields     │  ◄─── (Détruit récursivement les champs objets)
└────────────┬───────────────┘
             │
             ▼
┌────────────────────────────┐
│      Appel de free()       │  ◄─── (Libération de l'enveloppe parent)
└────────────────────────────┘
```

### B. Sécurité lors d'un échec de `initialize`
Si le constructeur `initialize` d'une classe lève une exception avant d'avoir terminé sa tâche, l'objet est considéré comme partiellement construit.
*   **Zéro-initialisation :** L'allocateur mémoire `INIT_OBJ` garantit que tous les champs de l'objet sont mis à `0` (null) dès l'allocation.
*   **Sûreté d'unwind :** Si une exception est levée dans le constructeur, le destructeur utilisateur `finalize` **n'est pas appelé**, car il s'attend à manipuler un objet cohérent et complet. L'unwinding de la VM appelle exclusivement la méthode système `__drop_fields`. Grâce à la mise à zéro initiale, `__drop_fields` ignore les champs non encore initialisés et détruit les sous-objets qui avaient déjà été alloués, évitant ainsi tout crash du runtime.

### C. Gestion des Sorties Hâtives (ScopeStack)
Le compilateur maintient une pile d'analyse lexicale (`ScopeStack`). Lorsqu'une instruction de saut ou de retour anticipé est compilée (`return`, `break`, `next`), le compilateur déroule cette pile et injecte explicitement les instructions `DROP` des variables du scope en cours de fermeture juste avant l'opcode de saut (`RET` ou `JMP`).

---


## 5. Table Synthétique de Comportement des Types

Ce tableau détaille le comportement physique de chaque entité de Volt au runtime :

| Entité | Allocation mémoire | Context `self` | Dispatch principal | Gestion de cycle de vie (RAII) | Vecteur de compilation |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **`struct`** | Pile (Stack-Allocated) | Oui (sur l'adresse) | Statique (*O(1)*) | Destruction automatique sur pile (no-op) | `COPY_BLOCK` / `CALL` direct |
| **`class`** | Tas (Heap) | Oui (pointeur) | Virtuel (Primary VTable) | `finalize` utilisateur + `__drop_fields` | `INIT_OBJ` / `CALL_METHOD` / `DROP` |
| **`module`** | Aucune (Static container) | Non | Statique (*O(1)*) | Aucun (pas d'instance) | `CALL` direct |
| **`mixin`** | Aucune (Trait) | Oui (sur l'hôte) | Dynamique (ITable) | Déléguée à la classe hôte | `CALL_MIXIN` |
| **`abstract`** | Impossible | Oui | Virtuel | Déléguée aux classes filles | Aucun (non instanciable) |
