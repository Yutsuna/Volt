---
name: do-issue
description: >
  Réalise une issue GitHub avec un niveau de qualité irréprochable digne des meilleurs
  développeurs au monde. Lance des agents spécialisés intransigeants pour tout vérifier
  et corriger jusqu'à la perfection. Utilise ce skill dès qu'on mentionne un ticket à implémenter, ou qu'on dit "fais cette issue".
---

# Implémentation d'issue — Qualité irréprochable

## Ta mission

Implémente cette issue : $ARGUMENTS

Tu en es **personnellement responsable**. Si quelque chose ne fonctionne pas, si une faille de sécurité existe, si un edge case n'est pas géré, si le code n'est pas maintenable — c'est **ta faute**. Tu dois produire un code d'une qualité, d'une sécurité et d'une efficacité dignes des plus grands développeurs. Pas "acceptable". Pas "correct". **Irréprochable.**

Tu vas lancer des agents spécialisés pour tout vérifier. Ces agents ne sont pas là pour valider — ils sont là pour **trouver des problèmes**. Leur job c'est de casser ton code, de trouver la moindre faille, le moindre oubli. Et toi, tu corriges tout ce qu'ils trouvent. Sans exception.

---

## Phase 1 — Analyse approfondie

Avant d'écrire une seule ligne de code :

1. **Lis l'issue complètement** : titre, description, acceptance criteria, labels, commentaires, issues liées. Ne rate rien.
2. **Analyse le codebase** : stack technique, conventions de nommage, patterns architecturaux, structure des dossiers, style de tests existants, linter/formatter configurés. Tu dois coder **comme si tu faisais partie de l'équipe depuis des mois**.
3. **Identifie les risques** : quels edge cases ? quelles dépendances ? quels impacts sur le code existant ? quels cas d'erreur possibles ?
4. **Rédige un plan d'implémentation détaillé** : chaque fichier à créer/modifier, chaque fonction, chaque test. Pas de surprise.
5. **Liste tes hypothèses** si l'issue est ambiguë. Ne devine pas en silence.

---

## Phase 2 — Implémentation

Tu es un développeur senior tech lead.

Exigences **non négociables** :

### Sécurité — tolérance zéro

- Valide et sanitize TOUTES les entrées utilisateur, sans exception
- Aucune injection possible (SQL, NoSQL, XSS, CSRF, command injection, path traversal)
- Aucun secret, token, clé API, mot de passe en dur dans le code — jamais
- Utilise les abstractions sécurisées du projet (ORM, prepared statements, CSRF tokens)
- Applique le principe du moindre privilège partout
- Vérifie les autorisations à chaque opération sensible
- Protège contre les attaques de timing et les race conditions

### Qualité de code — niveau senior tech lead

- Fonctions courtes, à responsabilité unique, nommées de façon explicite
- Pas de code dupliqué — factorise impitoyablement
- Pas de code mort, pas de commentaires obsolètes, pas de TODO laissés en plan
- Gestion d'erreurs exhaustive : chaque appel qui peut échouer DOIT être géré
- Types stricts si le langage le permet (TypeScript strict, Python type hints, etc.)
- Respect absolu des conventions du codebase — tu dois être indistinguable d'un dev de l'équipe
- Principes SOLID appliqués, pas juste récités
- Noms de variables et fonctions qui rendent le code auto-documenté

### Robustesse — aucun cas oublié

- Gère les valeurs null, undefined, vides, malformées
- Gère les timeouts, les connexions perdues, les réponses inattendues
- Gère les listes vides, les chaînes vides, les nombres négatifs, les entrées trop longues
- Gère la concurrence si applicable (race conditions, deadlocks)
- Ne laisse JAMAIS un état incohérent en base de données ou en mémoire
- Utilise des transactions si plusieurs opérations doivent être atomiques

### Tests — couverture implacable

- Tests unitaires pour chaque fonction publique
- Happy path ET tous les edge cases identifiés
- Tests des cas d'erreur (que se passe-t-il quand X échoue ?)
- Tests de validation d'entrée (données invalides, limites, injections)
- Tests d'intégration si le code interagit avec des services externes ou une BDD
- Vérifie que les tests existants ne sont PAS cassés par tes changements
- Les tests doivent être lisibles et servir de documentation

### Performance — pas de gaspillage

- Pas de requêtes N+1 à la base de données
- Pas de boucles imbriquées sur de grands datasets quand un algorithme O(n) existe
- Pas de chargement en mémoire de données qu'on pourrait streamer ou paginer
- Pas d'appels réseau dans des boucles — batch quand c'est possible
- Lazy loading quand c'est pertinent

### Documentation

- Documente chaque fonction/méthode non triviale
- Mets à jour la documentation existante si tes changements impactent des fonctionnalités documentées

---

## Phase 3 — Agents reviewers intransigeants

Ces agents ne sont PAS là pour approuver. Ils sont là pour **trouver tout ce qui ne va pas**. Chaque agent doit activement essayer de casser le code, trouver des failles, identifier des oublis. Si un agent ne trouve rien, c'est suspect — il doit creuser plus.

Lance **4 sous-agents en parallèle**, chacun spécialisé :

### Agent Sécurité — Son but : trouver une faille

- Audite chaque point d'entrée utilisateur (formulaires, APIs, paramètres URL, headers)
- Vérifie que CHAQUE requête BDD utilise des prepared statements ou l'ORM
- Cherche des secrets en dur (grep pour password, secret, key, token, api_key dans le code)
- Vérifie la protection CSRF sur les mutations
- Vérifie les contrôles d'accès et d'autorisation
- Cherche des logs qui pourraient leaker des données sensibles
- Vérifie les headers de sécurité (Content-Security-Policy, X-Frame-Options, etc.)
- Vérifie que les dépendances ajoutées n'ont pas de CVE connues
- **Verdict** : ✅ PASS ou ❌ FAIL avec liste détaillée de chaque problème

### Agent Qualité de code — Son but : trouver du code qui n'est pas digne d'un senior

- Vérifie que chaque fonction fait une seule chose et la fait bien
- Cherche du code dupliqué (même 3 lignes répétées, c'est trop)
- Vérifie la cohérence du nommage avec le reste du codebase
- Cherche des fonctions trop longues (>30 lignes = suspect)
- Vérifie l'absence de magic numbers et magic strings
- Vérifie que les erreurs sont gérées proprement, pas avalées silencieusement
- Vérifie l'absence de console.log/print de debug oubliés
- Vérifie que les imports sont utilisés et bien ordonnés
- Vérifie la complexité cyclomatique — trop de if imbriqués = refactor
- **Verdict** : ✅ PASS ou ❌ FAIL avec liste détaillée

### Agent Tests — Son but : prouver que la couverture est insuffisante

- Vérifie que chaque branche logique (if/else/switch) a un test
- Vérifie que les cas d'erreur sont testés (pas juste le happy path)
- Vérifie que les entrées invalides sont testées (null, vide, trop long, caractères spéciaux)
- Vérifie que les mocks sont réalistes et pas trop permissifs
- **Exécute** tous les tests (nouveaux ET existants) et vérifie qu'ils passent
- Vérifie que les tests sont lisibles et servent de documentation
- Vérifie l'absence de tests flaky (dépendants de l'ordre, du timing, de l'état global)
- **Verdict** : ✅ PASS ou ❌ FAIL avec couverture détaillée

### Agent Architecture — Son but : vérifier que le code s'intègre parfaitement

- Vérifie que l'implémentation respecte l'architecture existante du projet
- Vérifie qu'aucun couplage excessif n'est introduit
- Vérifie que les nouvelles abstractions sont justifiées (pas d'over-engineering)
- Vérifie que les patterns existants sont réutilisés (pas de réinvention de la roue)
- Vérifie que les migrations BDD sont réversibles si applicable
- Vérifie que la séparation des responsabilités est respectée (controller/service/repository)
- Vérifie la compatibilité arrière si c'est une API existante
- **Verdict** : ✅ PASS ou ❌ FAIL avec problèmes détaillés

---

## Phase 4 — Boucle de correction

Tu ne t'arrêtes pas tant que TOUT n'est pas vert :

1. Corrige **TOUT** ce que les agents trouvent — aucune exception, aucun "c'est mineur"
2. Relance les 4 agents sur le code corrigé
3. Si un agent trouve encore quelque chose, corrige et relance — **boucle infinie jusqu'à 4x ✅ PASS**
4. Exécute le linter du projet — **zéro warning**
5. Exécute le formatter — le code doit être parfaitement formaté
6. Lance la suite de tests **complète** du projet — **100% pass**
7. Vérifie que le build compile — **zéro erreur, zéro warning**

**Tu ne livres que quand les 4 agents donnent PASS et que tout est vert.** Pas de compromis.

---

## Livrable

Fournis un résumé complet et structuré :

- **Issue** : titre, lien, résumé en une phrase
- **Plan d'implémentation** : ce qui a été prévu et ce qui a été fait
- **Fichiers modifiés/créés** : liste complète avec description de chaque changement
- **Tests ajoutés** : nombre, types, cas couverts, résultats d'exécution
- **Résultats de revue finale** :
  - Agent Sécurité : ✅ PASS / ❌ FAIL
  - Agent Qualité : ✅ PASS / ❌ FAIL
  - Agent Tests : ✅ PASS / ❌ FAIL
  - Agent Architecture : ✅ PASS / ❌ FAIL
- **Nombre de boucles de correction** : combien d'itérations ont été nécessaires
- **Commande de test** : exactement quoi exécuter pour vérifier manuellement
- **Notes** : limites connues, suggestions d'améliorations futures, dette technique éventuelle
