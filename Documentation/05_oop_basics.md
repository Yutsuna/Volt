# Object-Oriented Programming: Structs and Classes

Volt organizes object-oriented code into two primary constructs: `struct` (value types) and `class` (reference types). Their core differences lie in memory layout, allocation strategies, and assignment semantics.

---

## 1. Structs vs. Classes Memory Representation

```
        [ VM Stack Frame ]                     [ Heap memory ]
     ┌────────────────────────┐             ┌───────────────────┐
     │ R1: [ Ptr Struct ] ────┼──┐          │ 0x00: Header      │
     ├────────────────────────┤  │          │       (TypeID...) │
     │ Spilled Struct Data    │◄─┘          ├───────────────────┤
     │ (Flat raw words)       │             │ 0x08: Field 1     │
     └────────────────────────┘             │ 0x10: Field 2     │
                                            └───────────────────┘
```

### Struct (Value Semantics)
*   **Memory Location**: Inline on the VM execution stack (stack-allocated).
*   **Header Overhead**: 0 bytes. Structs contain only their raw field values with no runtime metadata or TypeID.
*   **Assignment Semantics**: Copy-on-assignment. Assigning a struct to another variable, passing it to a function, or storing it in a class instance copies the entire block of memory physically using the `COPY_BLOCK` instruction.
*   **Method Dispatch**: Resolved statically at compile time. Method calls are compiled directly into static `CALL` instructions targeting a constant function index.

### Class (Reference Semantics)
*   **Memory Location**: Dynamically allocated on the heap using the `INIT_OBJ` instruction.
*   **Header Overhead**: 8 bytes (64 bits). Every class instance on the heap starts with a header containing a 32-bit `TypeID` (representing its type registry entry) and lifecycle flags.
*   **Assignment Semantics**: Reference copy. Assigning a class instance to another variable copies the 64-bit heap pointer. Both variables point to the same underlying heap object.
*   **Method Dispatch**: Resolved dynamically. Calls are compiled to `CALL_METHOD` which lookups the method target inside the class's VTable at runtime using its `TypeID`.

---

## 2. Defining Structs and Classes

### Struct Definition
```volt
struct Point
  x : Int64
  y : Int64

  def initialize(@x : Int64, @y : Int64)
  end

  def distance_squared(other : Point) -> Int64
    dx = @x - other.x
    dy = @y - other.y
    dx * dx + dy * dy
  end
end

p1 = Point.new(10, 20)
p2 = Point.new(15, 25)
d = p1.distance_squared(p2)
```

### Class Definition
```volt
class FileStream
  path : String
  handle : Int64

  def initialize(@path : String)
    @handle = open_system_handle(@path)
  end

  def finalize
    close_system_handle(@handle)
  end
end
```

---

## 3. Object Lifecycle and RAII Memory Management

Volt manages memory dynamically without a garbage collector. Instead, it relies on static lifetime analysis and Resource Acquisition Is Initialization (RAII).

### Constructors (`initialize`)
When `Type.new(...)` is invoked:
1.  **For Classes**: The VM allocates a zero-initialized block of heap memory of the class size (`INIT_OBJ`) and populates the `TypeID` header.
2.  **For Structs**: The VM reserves inline stack space.
3.  The `initialize` method is executed to populate fields. Instance variables are accessed inside the constructor (and methods) using the `@` prefix (e.g., `@x = x` or `@x`).

### Destructors (`finalize`)
When a class reference goes out of scope (e.g., when a function returns or a block exits), the compiler automatically inserts a `DROP` instruction.
The destruction sequence of a class instance is as follows:

```
[ DROP instruction called on object ]
                 │
                 ▼
     ┌───────────────────────┐
     │  Execute finalize()   │  ◄── (User-defined cleanup logic)
     └───────────┬───────────┘
                 │
                 ▼
     ┌───────────────────────┐
     │ Call __drop_fields()  │  ◄── (Compiler-generated deep drop)
     └───────────┬───────────┘
                 │
                 ▼
     ┌───────────────────────┐
     │     Call free()       │  ◄── (Release heap block)
     └───────────────────────┘
```

1.  **User Destructor**: The custom `finalize` method is called if defined.
2.  **Compiler Deep Drop (`__drop_fields`)**: The compiler automatically generates a `__drop_fields` routine for every class. This routine traverses all fields of reference types (classes) and emits a recursive `DROP` instruction for each one. Pointers that are `nil` are safely bypassed.
3.  **Heap Free**: The underlying heap memory wrapper is deallocated.

### Exception Safety and Unwinding
If an exception is raised inside a class constructor (`initialize`), the object is partially constructed. To prevent memory leaks of already-initialized fields:
*   The memory is guaranteed to be zero-initialized (`nil`) by `INIT_OBJ` prior to running the constructor.
*   If an exception occurs during initialization, the custom user `finalize` is **not** called (since it expects a fully coherent object).
*   Instead, VM unwinding invokes exclusively the system-generated `__drop_fields` routine. Because non-initialized fields are still `nil`, `__drop_fields` safely ignores them and releases only the fields that were successfully initialized before the exception occurred.

---

## 4. Class Inheritance

Volt supports single inheritance for classes. All abstract classes must be prefixed with the capital letter `A` (e.g., `AAnimal`).

```volt
abstract class AAnimal
  name : String

  def initialize(@name : String)
  end

  abstract def speak -> String
end

class Dog < AAnimal
  def speak -> String
    "Woof! My name is " + @name
  end
end

dog = Dog.new("Fido")
dog.speak() # => "Woof! My name is Fido"
```

### Layout inheritance
A subclass layout begins with the exact field sequence of its parent class. This layout preservation guarantees that inherited fields reside at the identical byte offsets, whether the VM accesses the parent or the child reference, enabling direct access without dynamic field offsets resolution.

### The implicit `Object` root
Every `class` that declares no superclass of its own implicitly derives from `Object` — there is nothing to write, it happens automatically:

```volt
class Device        # implicitly `class Device < Object`
  def ping -> Bool
    true
  end
end
```

`Object` carries no fields and no methods of its own; it exists purely so a parameter or variable can be declared to accept *any* class instance:

```volt
def log_anything(value : Object) -> Void
  puts(value.to_string)
end

log_anything(Device.new)   # any class satisfies `Object`
log_anything("a string")   # String < Object too
```

This is what lets the standard library's `Comparable` mixin (see [Modules, Mixins, and Member Visibility](06_modules_and_mixins.md)) declare `def <=>(other : Object) -> Int` and have it accept a value of any class, not just one specific type.
