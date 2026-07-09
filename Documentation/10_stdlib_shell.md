# The System::Shell Standard Library

Volt provides a strongly typed, object-oriented API for interacting with the operating system filesystem, subprocesses, and pipeline streams through the `System::Shell` namespace.

---

## 1. Filesystem Objects: Directory, Path, and File

Instead of exposing files and folders as raw strings, `System::Shell` models them as objects with guaranteed types and compile-time methods.

### Joining Paths
The `/` division operator is overloaded on `Path` and `Directory` objects to act as a platform-independent path joiner:
```volt
archive_dir = Directory.home / "archives" / "logs"
```

### Directory Traversal and Filtering
You can query and chain operations on directories using standard block closures:
```volt
# Traverses current directory to find large log files
Directory.current
  .files(recursive: true)
  .filter { |file| file.extension == "log" and file.size > 10.megabytes }
  .each { |file|
    destination = Directory.home / "archives" / file.name
    file.move_to(destination)
    Console.write_line("Archived: " + file.name + " (" + file.size.to_human_string() + ")")
  }
```

### Direct Globbing
Find files using standard glob matching syntax:
```volt
Path.glob("**/*.tmp").each { |file| file.delete() }
```

---

## 2. Process Control and Execution

Volt handles external program execution via structured `Proc` classes rather than raw shells commands strings.

### Spawning a Subprocess
Use `Proc.spawn` to start a process. It returns a handler that tracks process status, CPU usage, and handles streams.
```volt
pid = Proc.spawn("git status")
status = pid.wait()

if status.success?
  first_line = status.stdout.lines.first
  Console.write_line("Git branch: " + first_line)
end
```

---

## 3. Shell Pipelines and Chaining

To compose shell pipelines safely, use the `Shell.pipe` static utility. This chains the standard output of one command directly into the standard input of the next inside the VM, avoiding subshell invocation overhead:

```volt
out = Shell.pipe(
  "find . -name '*.volt'",
  "xargs wc -l"
)

Console.write_line("Volt lines counts: " + out.stdout.read_all())
```

---

## 4. Design Goals vs. Traditional Shells (PowerShell/Bash)

The `System::Shell` library is designed to address common issues found in typical shell environments:

1.  **Strict Compile-time Checking**: Variables representing files, sizes, and paths are strongly typed. You cannot accidentally treat a directory object as a raw string without explicit casting.
2.  **No Implicit String Coercion**: Operations must be explicit, preventing silent failures or variable injection bugs common in bash scripts.
3.  **IDE Auto-Completion**: Since objects have strict typings, IDEs can resolve method listings (such as `.extension`, `.size`, or `.move_to`) automatically, decreasing development errors.
