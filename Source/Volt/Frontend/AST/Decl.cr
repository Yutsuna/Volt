module Volt::Frontend


  # @[Name]  |  @[Name( args )]
  class Annotation
    property name : String
    property args : Array( AExpr )
    property loc  : Span

    def initialize( @name : String, @args : Array( AExpr ), @loc : Span )
    end
  end


  # A single function / method parameter.
  # `is_ivar` marks the constructor shorthand `def initialize( @x : T )`.
  class Param
    property name     : String
    property type_ann : ATypeNode?
    property default  : AExpr?
    property loc      : Span
    property is_ivar  : Bool

    def initialize( @name : String, @type_ann : ATypeNode?, @default : AExpr?, @loc : Span, @is_ivar : Bool = false )
    end
  end


  # Field inside a type body:  name : Type  |  name : Type = default
  # `getter name : Type` / `setter name : Type` set the accessor flags.
  class FieldDecl < ADecl
    property name      : String
    property type_ann  : ATypeNode
    property value     : AExpr?
    property is_getter : Bool = false
    property is_setter : Bool = false

    def initialize( @name : String, @type_ann : ATypeNode, @value : AExpr?, loc : Span )
      super( loc )
    end
  end


  # Method / member access control (default `Public`).
  # `Protected` = callable from the same class and its subclasses (C++ sense);
  # `Private` = callable only on the instance itself.
  enum Visibility
    Public
    Protected
    Private
  end


  # def name[T]( params ) -> ReturnType \n body \n end
  # `abstract def` produces a body-less FuncDecl with `is_abstract` set.
  # `def self.name` (a static / module method) sets `is_static`.
  class FuncDecl < ADecl
    property name        : String
    property type_params : Array( String )
    property params      : Array( Param )
    property return_type : ATypeNode?
    property body        : Array( ANode )
    property annotations : Array( Annotation )
    property is_async    : Bool
    property is_abstract : Bool = false
    property is_static   : Bool = false
    property visibility  : Visibility = Visibility::Public

    def initialize( @name : String, @type_params : Array( String ),
                    @params : Array( Param ), @return_type : ATypeNode?,
                    @body : Array( ANode ), @annotations : Array( Annotation ),
                    @is_async : Bool, loc : Span )
      super( loc )
    end
  end


  # `@@name : Type [= default]` — a module/class variable declaration inside a
  # type body. Backed by process-global storage (modules have no instances).
  class ClassVarDecl < ADecl
    property name     : String
    property type_ann : ATypeNode
    property value    : AExpr?

    def initialize( @name : String, @type_ann : ATypeNode, @value : AExpr?, loc : Span )
      super( loc )
    end
  end


  # `extend self` inside a module body — makes the module's instance-style
  # methods callable as static members on the module itself.
  class ExtendSelfDecl < ADecl
    def initialize( loc : Span )
      super( loc )
    end
  end


  # include Mixin  (body-level form, inside a `class` body)
  class IncludeDecl < ADecl
    property name : String

    def initialize( @name : String, loc : Span )
      super( loc )
    end
  end


  # [abstract] class Name[T] < Superclass include Mixin \n body \n end
  class ClassDecl < ADecl
    property name        : String
    property type_params : Array( String )
    property superclass  : String?
    property mixins      : Array( String )
    property body        : Array( ANode )
    property annotations : Array( Annotation )
    property is_abstract : Bool

    def initialize( @name : String, @type_params : Array( String ),
                    @superclass : String?, @mixins : Array( String ),
                    @body : Array( ANode ), @annotations : Array( Annotation ),
                    @is_abstract : Bool, loc : Span )
      super( loc )
    end
  end


  # struct Name[T] \n body \n end : value type: fields + methods, no inheritance
  class StructDecl < ADecl
    property name        : String
    property type_params : Array( String )
    property mixins      : Array( String )
    property body        : Array( ANode )
    property annotations : Array( Annotation )

    def initialize( @name : String, @mixins : Array( String ), @body : Array( ANode ),
                    @annotations : Array( Annotation ), loc : Span,
                    @type_params : Array( String ) = [] of String )
      super( loc )
    end
  end


  # module Name \n body \n end : static namespace: no instances, no dispatch
  class ModuleDecl < ADecl
    property name : String
    property body : Array( ANode )

    def initialize( @name : String, @body : Array( ANode ), loc : Span )
      super( loc )
    end
  end


  # mixin Name \n body \n end
  class MixinDecl < ADecl
    property name : String
    property body : Array( ANode )

    def initialize( @name : String, @body : Array( ANode ), loc : Span )
      super( loc )
    end
  end


  # component Name( params ) \n body \n end
  class ComponentDecl < ADecl
    property name     : String
    property params   : Array( Param )
    property body     : Array( ANode )
    property is_async : Bool

    def initialize( @name : String, @params : Array( Param ),
                    @body : Array( ANode ), @is_async : Bool, loc : Span )
      super( loc )
    end
  end


  # use System::Shell
  class UseDecl < ADecl
    property path : String

    def initialize( @path : String, loc : Span )
      super( loc )
    end
  end


  # @[External("libc")]  def name( params ) -> ReturnType
  class ExternDecl < ADecl
    property lib         : String?
    property name        : String
    property params      : Array( Param )
    property return_type : ATypeNode
    property extern_name : String? = nil

    def initialize( @lib : String?, @name : String, @params : Array( Param ),
                    @return_type : ATypeNode, loc : Span )
      super( loc )
    end
  end


  # circuit "Name" { runtime "..." ; entrypoint "..." ; modules( "A" => "dir", ... ) }
  # Project manifest declaration. Parsed like any other Volt source, but never
  # compiled/executed by the VM — consumed at project-load time by Circuit::Loader.
  class CircuitDecl < ADecl
    property name       : String
    property runtime    : String?
    property entrypoint : String?
    property modules    : HashLiteralExpr?

    def initialize( @name : String, @runtime : String?, @entrypoint : String?,
                    @modules : HashLiteralExpr?, loc : Span )
      super( loc )
    end
  end


  # @[Link("ModuleName")]  — file-level annotation (not attached to a decl)
  # telling the resolver to load the manifest module `ModuleName` before this file.
  class LinkDecl < ADecl
    property module_name : String

    def initialize( @module_name : String, loc : Span )
      super( loc )
    end
  end


end
