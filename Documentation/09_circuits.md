# Project Manifests and Circuits

In Volt, a multi-file program is organized as a **Circuit**. The layout, logical modules, and dependencies of a circuit are managed by a project manifest file named `Project.vl` located at the root of the project.

---

## 1. The `Project.vl` Manifest

The manifest is written in Volt syntax itself. It contains a single top-level `circuit` block defining the application's configuration:

```volt
circuit "MyApplication"
{
  runtime "0.1.0"
  entrypoint "Source/Main.vl"

  modules(
    "Core"      => "Source/Core",
    "Auth"      => "Source/Auth",
    "Database"  => "Source/Database"
  )
}
```

### Manifest Structure
*   `circuit "Name"`: Defines the logical name of the application scope.
*   `runtime "version"`: Enforces the target Volt runtime specification version.
*   `entrypoint "path"`: Points to the main executable source file.
*   `modules(...)`: Declares a key-value mapping (Hash) between logical namespaces used in the source code (keys) and their physical directory paths on disk relative to the root (values).

---

## 2. Linking Modules with `@[Link]`

To use a logical module within any `.vl` or `.vlx` source file, you declare a file-level link annotation at the top of the file:

```volt
@[Link("Database")]
@[Link("Core")]

config = Core::AppConfig.new(5432, "secret")
db = Database::DbClient.new(config)
db.connect()
```

When the compiler parses this file, it notes the dependencies on the `"Database"` and `"Core"` modules. The resolver then maps these names to their respective directories (e.g. `Source/Database` and `Source/Core`) as defined in the `Project.vl` manifest.

---

## 3. The Resolution Pipeline

When executing or type-checking a circuit (via `volt run` or `volt check` at the project root), the Volt compiler uses a multi-file resolver (`Circuit::Resolver`):

```
       [ Read entrypoint ]
                │
                ▼
       [ Parse AST & find @[Link] ]
                │
                ▼
  [ Resolve modules via Project.vl ]
                │
                ▼
 [ Tri-Topologique & Détection de Cycles ]
                │
                ▼
[ Build unified Program & Type Check ]
```

1.  **Parsing Entrypoint**: The compiler reads and parses the entrypoint file.
2.  **Dependency Gathering**: It inspects the file-level annotations to discover required modules.
3.  **Directory Scans**: For each required module, the compiler recursively reads all `.vl` and `.vlx` files located inside its mapped directory path.
4.  **Topological Sorting**: The compiled files are sorted topologically. Files belonging to dependency modules are sorted first, with the primary entrypoint file compiled last.
5.  **Diamond Dependencies Resolution**: If multiple modules depend on a shared module (e.g. both `Auth` and `Database` depend on `Core`), the resolver deduplicates the files to ensure the shared module is only parsed and initialized once.
6.  **Cycle Detection**: The compiler traverses the dependency graph using a Depth-First Search (DFS). If a circular reference is detected (e.g., `A` links to `B`, and `B` links to `A`), the compiler halts compilation and throws a cyclic dependency error diagnostic.
7.  **Directory Traversal Protection**: To enforce security boundaries, the resolver validates that all paths defined in the `modules` mapping remain confined inside the project root directory. Mappings containing parent directories traversal sequences (like `../`) are rejected during loading.

---

## 4. Manifest Synchronization with `volt circuit`

Maintaining the module mapping in `Project.vl` manually can become tedious as projects grow. Volt provides the `volt circuit` CLI command to automate this process.

### Creation Mode
If no `Project.vl` exists in the current working directory, running `volt circuit` will:
1.  Scan the first-level directories under `Source/`.
2.  Auto-detect folders containing `.vl` files.
3.  Set the entrypoint to `Source/Main.vl` if found.
4.  Generate a formatted `Project.vl` manifest file.

### Update Mode
If `Project.vl` already exists, running `volt circuit` will:
1.  Read and parse the existing configuration.
2.  Scan `Source/` for any new modules or deleted directories.
3.  Merge the changes, adding new modules and removing obsolete ones.
4.  Preserve custom configurations and battery dependencies.
5.  Write the synchronized manifest back to disk.
