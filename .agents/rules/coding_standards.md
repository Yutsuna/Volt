# Volt Coding Style Standards

**Version:** v0.1.0
**Status:** Core syntax rules are implemented; advanced features are deferred

## Core Syntax Rules
1. **Variables:** No keyword required.
   ```volt
   message = "Hello World"   # Inferred String
   age : Int64 = 28         # Explicit annotation (use Int64 for current implementation)
   ```
   Do not use `let`, `var`, `val`, or `const`. Currently supported types: Int64, Float64, Bool, String, Nil.

2. **Functions:** Defined with `def`, closed with `end`. Use `->` for return types, and `:` for parameter types.
   ```volt
   def add( a : Int64, b : Int64 ) -> Int64
     a + b
   end
   ```

3. **Blocks & Chaining:** Ruby/Crystal style. Use `end` to close multi-line blocks; curly braces `{ |x| ... }` are only allowed for inline/single-line lambdas.
   ```volt
   numbers.map { |it| it * 2 }.select { |it| it > 5 }
   ```
   Note: Full collection API (map, select) not yet implemented; basic function calls work.

4. **Boolean Operators:** English aliases preferred in prose code:
   * Use `and` instead of `&&`.
   * Use `or` instead of `||`.
   * Use `not` instead of `!`.
   All are currently supported and working.

5. **Classes, Mixins, Generics:** Use `mixin` for traits/mixins, `include` to import them, and `[T]` for generics.
   ```volt
   mixin Printable
     def to_s -> String
       "(Printable)"
     end
   end

   class Box[T] include Printable
     value : T
     def init(value : T)
       self.value = value
     end
   end
   ```
   Note: Classes, mixins, and generics are not yet implemented in the interpreter (v0.1.0).

6. **Components (Frontend):** Declare UI components with `component`, not `class` or `def`. Return JSX-like elements.
   ```volt
   component UserCard(user : User)
     return <div class="card">{ user.name }</div>
   end
   ```
   Note: Frontend components are not yet implemented.

7. **Shell API:** filesystem, processes, and IO are represented via the `System::Shell` namespace using strongly typed OOP calls (e.g. `Directory.current`, `Path.glob`, `Proc.spawn`).
   Note: System::Shell is not yet implemented; reserved for future development.

---

## Currently Supported Features (v0.1.0)

When writing Volt code for the current interpreter, use only:
- Primitive types: Int64, Float64, Bool, String, Nil
- Variable declaration with or without type annotation
- Function definitions with typed parameters and return types
- Arithmetic: +, -, *, /, %
- Comparison: <, <=, >, >=, ==, !=
- Logical: and, or, not
- Control flow: if/elsif/else, while, until
- Return statements
- Native function calls via @[External] annotation

Avoid using features not in this list as they are not yet implemented.
