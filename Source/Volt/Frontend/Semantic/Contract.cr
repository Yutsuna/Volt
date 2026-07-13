module Volt::Frontend


  class FuncSig
    property name      : String
    property params    : Array( Type )
    property ret       : Type
    property extern    : Bool
    property lib       : String?
    property decl_span : Span?
    property extern_name : String? = nil
    # Nominal owner (class/struct/mixin/module) of a method, or nil for a free
    # function. `is_static` marks methods resolved at compile time with no
    # receiver (module functions), as opposed to instance methods.
    property owner     : String?
    property is_static : Bool
    property visibility : Visibility

    def initialize( @name, @params, @ret, @extern = false, @lib = nil, @decl_span = nil,
                    @owner = nil, @is_static = false, @visibility = Visibility::Public )
    end
  end


  # The flavour of a user-defined nominal declaration.
  enum NominalKind
    Class
    Struct
    Mixin
    Module
  end


  # The fully resolved description of one user-defined type, produced by the
  # semantic collection pass (Phase B) and consumed by the bytecode compiler.
  # In Phase 0 this is the frozen contract; later phases populate `layout`,
  # `methods`, vtables and itables.
  class TypeInfo
    property kind        : NominalKind
    property name        : String
    property type_id     : Int32
    property layout      : TypeLayout?
    property reg_layout  : TypeLayout?
    property superclass  : String?
    property mixins      : Array( String )
    property is_abstract : Bool
    property methods     : Hash( String, FuncSig )
    # The compiler needs the AST body behind each signature to lower it :
    # `TypedProgram#methods` is a flat, owner-less list, so this is the only
    # place that keeps method name -> declaration paired with its owner.
    property methods_ast : Hash( String, FuncDecl )
    # The user `initialize` / `finalize` signatures, when declared. Named
    # `initializer` / `finalizer` to avoid clashing with Crystal's own
    # `initialize` / `finalize` methods.
    property initializer : FuncSig?
    property finalizer   : FuncSig?
    # Every user `initialize` overload keyed by arity. `initializer` stays the
    # first-declared one (a cheap "has any constructor" presence marker for
    # the single-overload fast path); arity-aware resolution goes through
    # this hash. When a type declares more than one overload, its
    # `methods`/`methods_ast` entries are keyed `initialize/<arity>` so each
    # overload gets its own compiled chunk.
    property initializers : Hash( Int32, FuncSig )
    # Static dispatch table for a class : method name -> slot index. Built by
    # `TypeCollector#build_vtable` by copying the superclass's layout verbatim
    # (an override reuses its inherited index) and appending each mixin's and
    # each own new method name. A struct/mixin/module never populates this —
    # only classes have real polymorphism (architecture #7.3).
    property vtable_layout : Hash( String, Int32 )
    property vtable_size   : Int32
    # `@@name` module/class variables : name -> type. Backed by process-global
    # storage (a module has no instances); the compiler assigns each a global
    # slot. Declaration order is preserved for deterministic slot/init emission.
    property class_vars : Hash( String, Type )
    property extend_self : Bool = false

    def initialize( @kind, @name, @type_id,
                    @layout = nil, @superclass = nil,
                    @mixins = [] of String, @is_abstract = false,
                    @methods = {} of String => FuncSig,
                    @methods_ast = {} of String => FuncDecl,
                    @initializer = nil, @finalizer = nil,
                    @initializers = {} of Int32 => FuncSig,
                    @vtable_layout = {} of String => Int32, @vtable_size = 0,
                    @class_vars = {} of String => Type,
                    @extend_self = false)
    end
  end


  class TypedProgram
    property program    : Program
    property functions  : Array( FuncDecl )
    property top_level  : Array( ANode )
    property signatures : Hash( String, FuncSig )
    # Resolved nominal types (classes, structs, mixins, modules) keyed by name,
    # and the method bodies to lower with their owning type. Empty until the
    # semantic OOP collection pass (Phase B) fills them.
    property types   : Hash( String, TypeInfo )
    property methods : Array( FuncDecl )

    def initialize( @program, @functions, @top_level, @signatures,
                    @types = {} of String => TypeInfo, @methods = [] of FuncDecl )
    end
  end


end
