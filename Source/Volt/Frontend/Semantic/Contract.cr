module Volt::Frontend


  class FuncSig
    property name      : String
    property params    : Array( Type )
    property ret       : Type
    property extern    : Bool
    property lib       : String?
    property decl_span : Span?
    # Nominal owner (class/struct/mixin/module) of a method, or nil for a free
    # function. `is_static` marks methods resolved at compile time with no
    # receiver (module functions), as opposed to instance methods.
    property owner     : String?
    property is_static : Bool

    def initialize( @name, @params, @ret, @extern = false, @lib = nil, @decl_span = nil,
                    @owner = nil, @is_static = false )
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

    def initialize( @kind, @name, @type_id,
                    @layout = nil, @superclass = nil,
                    @mixins = [] of String, @is_abstract = false,
                    @methods = {} of String => FuncSig,
                    @methods_ast = {} of String => FuncDecl,
                    @initializer = nil, @finalizer = nil )
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
