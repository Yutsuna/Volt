module Volt::Frontend


  module Catalog


    def self.token_name( kind : TokenKind ) : String
      case kind
      when .end?       then "`end`"
      when .r_paren?   then "`)`"
      when .l_paren?   then "`(`"
      when .r_bracket? then "`]`"
      when .l_bracket? then "`[`"
      when .r_brace?   then "`}`"
      when .l_brace?   then "`{`"
      when .ident?     then "an identifier"
      when .colon?     then "`:`"
      when .comma?     then "`,`"
      when .eof?       then "end of file"
      when .newline?   then "a line break"
      else                  "`#{kind.to_s.downcase}`"
      end
    end

    def self.describe( tok : Token ) : String
      tok.kind.eof? ? "end of file" : "`#{tok.value}`"
    end


    # ------------------------------------------------------------------ parse (P00xx)
    module Parse
      extend self

      def expected( expected : TokenKind, got : Token ) : Diagnostic
        Diagnostic.error( "P0001", "expected #{Catalog.token_name( expected )}, found #{Catalog.describe( got )}" )
          .with_primary( got.span, "expected #{Catalog.token_name( expected )} here" )
      end

      def unclosed( expected : TokenKind, got : Token, opener : Span, opener_desc : String ) : Diagnostic
        Diagnostic.error( "P0002", "expected #{Catalog.token_name( expected )} to close #{opener_desc}, found #{Catalog.describe( got )}" )
          .with_primary( got.span, "expected #{Catalog.token_name( expected )} here" )
          .with_secondary( opener, "#{opener_desc} opened here" )
      end

      def unexpected_expr( tok : Token ) : Diagnostic
        Diagnostic.error( "P0003", "unexpected #{Catalog.describe( tok )} in expression" )
          .with_primary( tok.span, "not valid here" )
      end

      def unexpected_infix( tok : Token ) : Diagnostic
        Diagnostic.error( "P0004", "unexpected infix operator #{Catalog.describe( tok )}" )
          .with_primary( tok.span )
      end

      def expected_after( what : String, after : String, got : Token ) : Diagnostic
        Diagnostic.error( "P0005", "expected #{what} after `#{after}`, found #{Catalog.describe( got )}" )
          .with_primary( got.span, "expected #{what} here" )
      end

      def macro_expansion( message : String, span : Span ) : Diagnostic
        Diagnostic.error( "P0006", "macro expansion failed: #{message}" )
          .with_primary( span )
      end

      def unexpected_in_type_body( tok : Token ) : Diagnostic
        Diagnostic.error( "P0007", "unexpected #{Catalog.describe( tok )} in type body" )
          .with_primary( tok.span, "only fields, methods, and nested types are allowed here" )
      end
    end


    # ------------------------------------------------------------------ semantic (S00xx)
    module Sema
      extend self

      def unsupported_top_level( what : String, span : Span ) : Diagnostic
        Diagnostic.error( "S0001", "top-level `#{what}` is not supported in #{VERSION}" )
          .with_primary( span )
      end

      def unsupported_nested( what : String, span : Span ) : Diagnostic
        Diagnostic.error( "S0002", "nested `#{what}` is not supported in #{VERSION}" )
          .with_primary( span )
      end

      def duplicate_definition( name : String, span : Span, first : Span? ) : Diagnostic
        d = Diagnostic.error( "S0003", "duplicate definition of `#{name}`" )
          .with_primary( span, "redefined here" )
        d = d.with_secondary( first, "first defined here" ) if first
        d
      end

      def param_needs_type( param : String, func : String, span : Span ) : Diagnostic
        Diagnostic.error( "S0004", "parameter `#{param}` of `#{func}` needs a type annotation in #{VERSION}" )
          .with_primary( span )
          .with_help( "add an annotation, e.g. `#{param} : Int`" )
      end

      def unsupported_param_type( param : String, span : Span ) : Diagnostic
        Diagnostic.error( "S0005", "unsupported parameter type for `#{param}` in #{VERSION}" )
          .with_primary( span )
      end

      def unsupported_return_type( func : String, span : Span ) : Diagnostic
        Diagnostic.error( "S0006", "unsupported return type for `#{func}` in #{VERSION}" )
          .with_primary( span )
      end

      def undefined_variable( name : String, span : Span, suggestion : String? ) : Diagnostic
        d = Diagnostic.error( "S0007", "undefined variable `#{name}`" )
          .with_primary( span, "not found in this scope" )
        d = d.with_help( "did you mean `#{suggestion}`?" ) if suggestion
        d
      end

      def non_simple_assign( span : Span ) : Diagnostic
        Diagnostic.error( "S0008", "only simple `name = value` assignment is supported in #{VERSION}" )
          .with_primary( span )
      end

      def annotation_mismatch( name : String, declared : String, actual : String, span : Span ) : Diagnostic
        Diagnostic.error( "S0009", "type mismatch: `#{name}` declared #{declared} but value is #{actual}" )
          .with_primary( span )
      end

      def reassign_type( name : String, existing : String, actual : String, span : Span ) : Diagnostic
        Diagnostic.error( "S0010", "cannot reassign `#{name}` (#{existing}) with a #{actual}" )
          .with_primary( span )
      end

      def unsupported_annotation( span : Span ) : Diagnostic
        Diagnostic.error( "S0011", "unsupported type annotation in #{VERSION}" )
          .with_primary( span )
      end

      def binary_numeric( op : String, lt : String, rt : String, span : Span ) : Diagnostic
        Diagnostic.error( "S0012", "operator `#{op}` needs matching numeric operands, got #{lt} and #{rt}" )
          .with_primary( span )
      end

      def comparison_numeric( lt : String, rt : String, span : Span ) : Diagnostic
        Diagnostic.error( "S0013", "comparison needs matching numeric operands, got #{lt} and #{rt}" )
          .with_primary( span )
      end

      def incomparable( lt : String, rt : String, span : Span ) : Diagnostic
        Diagnostic.error( "S0014", "cannot compare #{lt} with #{rt}" )
          .with_primary( span )
      end

      def unsupported_operator( op : String, span : Span ) : Diagnostic
        Diagnostic.error( "S0015", "operator `#{op}` is not supported in #{VERSION}" )
          .with_primary( span )
      end

      def unary_numeric( ot : String, span : Span ) : Diagnostic
        Diagnostic.error( "S0016", "unary `-` needs a numeric operand, got #{ot}" )
          .with_primary( span )
      end

      def unsupported_unary( span : Span ) : Diagnostic
        Diagnostic.error( "S0017", "unary operator is not supported in #{VERSION}" )
          .with_primary( span )
      end

      def non_direct_call( span : Span ) : Diagnostic
        Diagnostic.error( "S0018", "only direct function calls `name(...)` are supported in #{VERSION}" )
          .with_primary( span )
      end

      def block_arg( span : Span ) : Diagnostic
        Diagnostic.error( "S0019", "block arguments are not supported in #{VERSION}" )
          .with_primary( span )
      end

      def undefined_function( name : String, span : Span, suggestion : String? ) : Diagnostic
        d = Diagnostic.error( "S0020", "call to undefined function `#{name}`" )
          .with_primary( span, "not found" )
        d = d.with_help( "did you mean `#{suggestion}`?" ) if suggestion
        d
      end

      def arity_mismatch( name : String, expected : Int32, got : Int32, span : Span ) : Diagnostic
        Diagnostic.error( "S0021", "`#{name}` expects #{expected} argument(s), got #{got}" )
          .with_primary( span )
      end

      def argument_type( idx : Int32, name : String, expected : String, actual : String, span : Span ) : Diagnostic
        Diagnostic.error( "S0022", "argument #{idx} of `#{name}` expects #{expected}, got #{actual}" )
          .with_primary( span )
      end

      def unsupported_expr( what : String, span : Span ) : Diagnostic
        Diagnostic.error( "S0023", "`#{what}` is not supported in #{VERSION}" )
          .with_primary( span )
      end

      def unresolved_extern( name : String, span : Span, available : Enumerable( String ) ) : Diagnostic
        Diagnostic.error( "S0024", "extern function `#{name}` has no native implementation in the interpreter" )
          .with_primary( span, "called here" )
          .with_help( "the interpreter only provides: #{available.join( ", " )}" )
      end

      def unknown_field_or_method( owner : String, name : String, span : Span ) : Diagnostic
        Diagnostic.error( "S0025", "`#{owner}` has no field or method named `#{name}`" )
          .with_primary( span )
      end

      def unknown_instance_var( owner : String, name : String, span : Span ) : Diagnostic
        Diagnostic.error( "S0026", "`#{owner}` has no instance variable `@#{name}`" )
          .with_primary( span )
      end

      def ivar_outside_method( name : String, span : Span ) : Diagnostic
        Diagnostic.error( "S0027", "`@#{name}` used outside of an instance method" )
          .with_primary( span )
      end

      def self_outside_method( span : Span ) : Diagnostic
        Diagnostic.error( "S0028", "`self` used outside of an instance method" )
          .with_primary( span )
      end

      def unknown_superclass( owner : String, name : String, span : Span ) : Diagnostic
        Diagnostic.error( "S0029", "`#{owner}` inherits from undefined class `#{name}`" )
          .with_primary( span )
      end

      def unknown_mixin( owner : String, name : String, span : Span ) : Diagnostic
        Diagnostic.error( "S0030", "`#{owner}` includes undefined mixin `#{name}`" )
          .with_primary( span )
      end

      def include_module_forbidden( name : String, span : Span ) : Diagnostic
        Diagnostic.error( "S0031", "module `#{name}` cannot be `include`d : modules are static namespaces, not mixins" )
          .with_primary( span )
          .with_help( "use `mixin` instead of `module` if `#{name}` is meant to be mixed into instances" )
      end

      def circular_type( name : String, span : Span ) : Diagnostic
        Diagnostic.error( "S0032", "circular definition involving `#{name}` (inheritance or struct composition)" )
          .with_primary( span )
      end

      def abstract_instantiation( name : String, span : Span ) : Diagnostic
        Diagnostic.error( "S0033", "cannot instantiate abstract class `#{name}`" )
          .with_primary( span )
      end

      def missing_abstract_impl( owner : String, method : String, from : String, span : Span ) : Diagnostic
        Diagnostic.error( "S0034", "`#{owner}` does not implement abstract method `#{method}` from `#{from}`" )
          .with_primary( span )
      end

      def private_method_call( owner : String, name : String, span : Span ) : Diagnostic
        Diagnostic.error( "S0035", "`#{owner}` has no accessible method named `#{name}`" )
          .with_primary( span, "private method called here" )
      end

      def private_field_access( owner : String, name : String, span : Span ) : Diagnostic
        Diagnostic.error( "S0036", "`#{owner}` has no accessible field named `#{name}`" )
          .with_primary( span )
      end

      def private_instance_var_access( owner : String, name : String, span : Span ) : Diagnostic
        Diagnostic.error( "S0037", "`#{owner}` has no accessible instance variable `@#{name}`" )
          .with_primary( span )
      end

      def unknown_type( name : String, span : Span ) : Diagnostic
        Diagnostic.error( "S0038", "unknown type `#{name}`" )
          .with_primary( span )
      end

      def unknown_type_param( name : String, span : Span ) : Diagnostic
        Diagnostic.error( "S0039", "unknown type parameter `#{name}`" )
          .with_primary( span )
      end

      def protected_method_call( owner : String, name : String, span : Span ) : Diagnostic
        Diagnostic.error( "S0040", "`#{owner}` has no accessible method named `#{name}`" )
          .with_primary( span, "protected method called here" )
      end

      def protected_field_access( owner : String, name : String, span : Span ) : Diagnostic
        Diagnostic.error( "S0041", "`#{owner}` has no accessible field named `#{name}`" )
          .with_primary( span )
      end

      def protected_instance_var_access( owner : String, name : String, span : Span ) : Diagnostic
        Diagnostic.error( "S0042", "`#{owner}` has no accessible instance variable `@#{name}`" )
          .with_primary( span )
      end

      def module_instantiation( name : String, span  : Span ) : Diagnostic
        Diagnostic.error( "S0043", "cannot instantiate module `#{name}` : modules are static namespaces" )
          .with_primary( span, "modules do not have instances" )
      end

      def mixin_instantiation( name : String, span  : Span ) : Diagnostic
        Diagnostic.error( "S0044", "cannot instantiate mixin `#{name}` : mixins are not classes" )
          .with_primary( span, "mixins do not have instances" )
      end

      def invalid_namespace_operator( module_name : String, member : String, span : Span ) : Diagnostic
        Diagnostic.error("S0045", "cannot access nested type `#{member}` from module `#{module_name}` using `.`")
          .with_primary( span )
          .with_help("use the namespace resolution operator `::`, e.g. `#{module_name}::#{member}`")
      end

      def struct_inheritance(name : String, span : Span) : Diagnostic
        Diagnostic.error("S0046", "struct `#{name}` cannot inherit from another type")
          .with_primary(span, "structs do not support inheritance in #{VERSION}")
      end

      def subclassing_struct(child : String, parent : String, span : Span) : Diagnostic
        Diagnostic.error("S0047", "class `#{child}` cannot inherit from struct `#{parent}`")
          .with_primary(span)
      end

      def finalize_has_arguments(span : Span) : Diagnostic
        Diagnostic.error("S0048", "destructor `finalize` cannot accept parameters")
          .with_primary(span, "must be defined as `def finalize -> Void` ou `def finalize`")
      end

      def finalize_returns_value(actual_type : String, span : Span) : Diagnostic
        Diagnostic.error("S0049", "destructor `finalize` cannot return a value, got #{actual_type}")
          .with_primary(span, "destructors must return Void")
      end

      def initialize_return_type(actual_type : String, span : Span) : Diagnostic
        Diagnostic.error("S0050", "constructor `initialize` must return an instance of the class, got #{actual_type}")
          .with_primary(span, "constructors must return an instance of the class")
      end

      def non_interpolable_type( actual_type : String, span : Span ) : Diagnostic
        Diagnostic.error( "S0051", "cannot interpolate value of type #{actual_type} into a string" )
          .with_primary( span, "only String, Char, and numeric types can be interpolated" )
      end

      def break_outside_loop( span : Span ) : Diagnostic
        Diagnostic.error( "S0052", "`break` used outside of a loop" )
          .with_primary( span )
      end

      def next_outside_loop( span : Span ) : Diagnostic
        Diagnostic.error( "S0053", "`next` used outside of a loop" )
          .with_primary( span )
      end
    end


  end


end
