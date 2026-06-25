# Volt Coding Style Standards

## Core Syntax Rules
1. **Variables:** No keyword required.
   ```volt
   message = "Hello World"   # Inferred String
   age : Int = 28            # Explicit annotation
   ```
   Do not use `let`, `var`, `val`, or `const`.

2. **Functions:** Defined with `def`, closed with `end`. Use `->` for return types, and `:` for parameter types.
   ```volt
   def add( a : Int, b : Int ) -> Int
     a + b
   end
   ```

3. **Blocks & Chaining:** Ruby/Crystal style. Use `end` to close multi-line blocks; curly braces `{ |x| ... }` are only allowed for inline/single-line lambdas.
   ```volt
   numbers.map { |it| it * 2 }.select { |it| it > 5 }
   ```

4. **Boolean Operators:** English aliases preferred in prose code:
   * Use `and` instead of `&&`.
   * Use `or` instead of `||`.
   * Use `not` instead of `!`.

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

6. **Components (Frontend):** Declare UI components with `component`, not `class` or `def`. Return JSX-like elements.
   ```volt
   component UserCard(user : User)
     return <div class="card">{ user.name }</div>
   end
   ```

7. **Shell API:** filesystem, processes, and IO are represented via the `System::Shell` namespace using strongly typed OOP calls (e.g. `Directory.current`, `Path.glob`, `Proc.spawn`).
