# Frontend Components and Volt JSX

Volt supports native frontend user interface development using component declarations and a JSX-like markup syntax. Frontend source files use the `.vlx` file extension.

---

## 1. Defining Components

Components are declared using the `component` keyword instead of `class` or `def`. This allows the Volt compiler to optimize compilation and layout rendering paths separately from standard logic.

### Basic Component
A component can accept input properties (Props) via typed parameters. The render block consists of inline JSX tags.

```vlx
component Header(title : String)
  <header class="app-header">
    <h1>{title}</h1>
  </header>
end
```

---

## 2. Component State Management

Components can declare a local reactive state using the `state` keyword, which accepts a typed structure with default initializers.

```vlx
component Counter(initial_value : Int64)
  state = { count : Int64 = initial_value }

  def increment
    state.count = state.count + 1
  end

  <div class="counter-box">
    <button on:click={-> increment}>+1</button>
    <span>Current Count: {state.count}</span>
  </div>
end
```

### State Modification
Modifying `state` properties triggers a component re-render. State values are accessed through the `state.` prefix.

---

## 3. JSX Syntax and Data Binding

Volt JSX matches standard HTML elements and supports expressions enclosed in single curly braces `{ ... }`.

### Property Bindings
Pass parameters to subcomponents or element attributes:
```vlx
<Header title="My Dashboard" />
<div class={container_class}>
  <input type="text" value={state.query} />
</div>
```

### Event Listeners
Events are wired using the `on:` namespace prefix. Event handlers receive a reference to a function or an inline parameterless closure (`-> handler`):
```vlx
<button on:click={-> do_something}>Submit</button>
```

### Conditional Rendering
In-line logical conditions or ternary expressions can select which markup elements to display:
```vlx
<div>
  {state.user ? <UserProfile user={state.user} /> : <LoginButton />}
</div>
```

### Fragment Shorthand
When a component returns multiple root elements, wrap them in empty tags `<>` and `</>` (Fragments) to satisfy single-root AST rules:
```vlx
component Layout()
  <>
    <Navigation />
    <MainContent />
    <Footer />
  </>
end
```

---

## 4. Async Components

To fetch remote database entries or trigger latency-bound network operations, components can be declared as asynchronous using `async component`.

```vlx
async component UserLoader(id : Int64)
  user = await fetch_user_data(id)

  <div class="profile-card">
    <h3>{user.name}</h3>
    <p>Email: {user.email}</p>
  </div>
end
```
During rendering, the VM schedules the component execution context on a separate fiber task, pausing execution until the `await` expression resolves, without blocking the main rendering loop.
