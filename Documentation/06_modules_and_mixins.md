# Modules, Mixins, and Member Visibility

Volt distinguishes between static symbol organization and dynamic behavior injection using `module` and `mixin`.

---

## 1. Modules (Static Namespaces)

A `module` acts as a static container for functions, constants, types, and class variables. Modules cannot be instantiated, do not participate in inheritance hiérarchies, and lack a dynamic `self` context in their methods.

### Module Syntax
```volt
module DatabaseConfig
  @@connection_limit : Int64 = 10
  @@current_connections : Int64 = 0

  def self.active_connections -> Int64
    @@current_connections
  end

  def self.connect -> Bool
    if @@current_connections < @@connection_limit
      @@current_connections = @@current_connections + 1
      return true
    end
    false
  end
end

# Usage:
connected = DatabaseConfig.connect()
active = DatabaseConfig.active_connections()
```

### Key Rules for Modules
*   **Static Context**: Module-level variables are declared with `@@` and are shared globally within the module scope.
*   **Static Methods**: Methods meant to be called on the module itself are prefixed with `self.` (e.g., `def self.connect`).
*   **Compilation Cost**: Module method calls are resolved statically at compile time and compile down to direct `CALL` instructions with no VM overhead.
*   **No Instantiation**: Writing `DatabaseConfig.new` or trying to include a module via `include DatabaseConfig` triggers a compile-time semantic error.

---

## 2. Mixins (Traits)

A `mixin` is an injectable trait containing instance methods that can be shared across multiple classes. Mixins allow Volt classes to implement multiple interfaces and reuse method logic without multiple inheritance.

### Mixin Syntax and Inclusion
```volt
mixin Loggable
  def log(message : String) -> Void
    # Print to console or standard output
    puts("[LOG] " + message)
  end
end

mixin Authenticating
  def authenticate(user : String) -> Bool
    log("Authenticating user: " + user)
    user == "admin"
  end
end

# Injecting mixins into a class:
class ControlConsole include Loggable include Authenticating
  session_id : String

  def initialize(@session_id : String)
  end

  def run_cmd(user : String, command : String) -> Void
    if authenticate(user)
      log("Executing command: " + command)
    else
      log("Access denied for: " + user)
    end
  end
end
```

### Accessing Host Fields
Mixins can access instance variables of their host classes (e.g., `@session_id`). During compile-time semantic checks, the analyzer validates that any class including the mixin possesses the required fields at compile-time offsets.

---

## 2b. Standard Library Mixins: `Comparable` and `Inspectable`

`Lib/Mixins/` ships two mixins used throughout the standard library:

*   **`Inspectable`** provides `inspect -> String`, formatted as `"<TypeName: to_string-output>"`, built on whatever `to_string` the including type already defines.
*   **`Comparable`** provides `<`, `<=`, `>`, `>=`, and `==`, all implemented in terms of a single `abstract def <=>(other : Object) -> Int` that the including type must supply:

```volt
mixin Comparable
  def <( other : Object ) -> Bool
    comp = self <=> other
    comp ? comp < 0 : false
  end
  # ... <=, >, >=, == follow the same pattern

  abstract def <=>( other : Object ) -> Int
end

class Version
  include Comparable
  major : Int
  minor : Int

  def initialize(@major : Int, @minor : Int)
  end

  def <=>( other : Version ) -> Int
    return @major - other.major unless @major == other.major
    @minor - other.minor
  end
end

Version.new(1, 2) < Version.new(1, 5)   # => true, via Comparable#<
```

`other : Object` is what lets `<=>` be declared once per type yet still satisfy every comparison method the mixin generates — see [The implicit `Object` root](05_oop_basics.md#the-implicit-object-root).

**Numeric types are the one exception**: `Int`/`Float` also `include Comparable` (so they get `==`), but their `<`, `>`, `<=`, `>=` always resolve to the native hardware comparison instead of the mixin's version — never the other way around. This matters because `Int#<=>` is itself written in terms of `<`/`>` (`self < other ? -1 : ...`); if those went back through `Comparable`'s `<` (which calls `<=>`), it would recurse forever. The compiler special-cases this: **a comparison between two numeric operands always takes the native path**, regardless of what mixins are included.

---

## 3. Under the Hood: VTables vs. ITables

To resolve method offsets without multiple inheritance layout collisions, Volt implements two levels of method dispatch tables in the Virtual Machine:

### VTable (Virtual Table)
*   Used for direct class inheritance.
*   The compiler resolves method offsets linearly. Since subclasses copy parent layout prefixes, parent class methods have identical offsets in both parent and child VTables.
*   Dispatched using `CALL_METHOD` at runtime, which is a fast constant-time lookup.

### ITable (Interface Table)
*   Used when calling a method on a mixin interface or calling a mixin method inside a class.
*   Because different classes can include the same mixin at different hierarchy levels and physical offsets, the compiler cannot assign a single static offset to mixin methods.
*   At runtime, each class that includes one or more mixins is equipped with an ITable.
*   The ITable is represented in the VM Tier-0 interpreter as a contiguous array of pairs:
    ```
    ITable: [ { MixinModuleID_1, VTablePtr_1 }, { MixinModuleID_2, VTablePtr_2 } ]
    ```
*   When a mixin method is invoked via `CALL_MIXIN`, the VM performs a linear search through the host object's ITable matching the `MixinModuleID`. Once resolved, it forwards execution to the target offset inside the matched mixin's VTable.

---

## 4. Member Visibility

Volt supports visibility modifiers to control access to methods and instance fields.

*   **Public (Default)**: All classes, structures, and methods are public by default.
*   **Private**: Restricts visibility of methods or fields to the defining class or module.

```volt
class BankAccount
  balance : Float64          # Public field

  private def encrypt_token(token : String) -> String
    # Only callable within BankAccount methods
    token + "_secret"
  end
end
```
*Note: Any external attempt to call a private method or modify a private field raises a semantic compilation diagnostics error.*
