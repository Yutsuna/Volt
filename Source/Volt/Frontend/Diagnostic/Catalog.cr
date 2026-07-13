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

      def expected_one_of( what : String, got : Token ) : Diagnostic
        Diagnostic.error( "P0008", "expected #{what}, found #{Catalog.describe( got )}" )
          .with_primary( got.span, "expected #{what} here" )
      end

      def circuit_no_interpolation( span : Span ) : Diagnostic
        Diagnostic.error( "P0009", "string interpolation is not allowed inside a `circuit` manifest" )
          .with_primary( span, "manifest values must be plain string literals" )
      end

      def link_expects_module_name( span : Span ) : Diagnostic
        Diagnostic.error( "P0010", "`@[Link]` expects exactly one plain string argument" )
          .with_primary( span, %(use `@[Link("ModuleName")]`) )
      end

      def duplicate_type_param( name : String, got : Token ) : Diagnostic
        Diagnostic.error( "P0011", "duplicate type parameter `#{name}`" )
          .with_primary( got.span, "`#{name}` is already declared on this `def`" )
      end
    end


    # ------------------------------------------------------------------ circuit (C00xx)
    module Circuit
      extend self

      def manifest_not_found( path : String ) : Diagnostic
        Diagnostic.error( "C0001", "circuit manifest `#{path}` not found" )
          .with_help( "run `volt circuit` at the project root to generate one" )
      end

      def no_circuit_block( file : String ) : Diagnostic
        Diagnostic.error( "C0002", "no `circuit` block found in `#{file}`" )
          .with_help( "a manifest must contain exactly one `circuit \"Name\" { ... }` block" )
      end

      def multiple_circuit_blocks( span : Span, first : Span ) : Diagnostic
        Diagnostic.error( "C0003", "multiple `circuit` blocks in manifest" )
          .with_primary( span, "second `circuit` block here" )
          .with_secondary( first, "first `circuit` block here" )
      end

      def missing_entry( entry : String, span : Span ) : Diagnostic
        Diagnostic.error( "C0004", "circuit is missing its `#{entry}` entry" )
          .with_primary( span, "add `#{entry} \"...\"` inside this circuit block" )
      end

      def entrypoint_not_found( path : String, span : Span ) : Diagnostic
        Diagnostic.error( "C0005", "entrypoint `#{path}` does not exist" )
          .with_primary( span, "no such file relative to the project root" )
      end

      def duplicate_module( name : String, span : Span, first : Span ) : Diagnostic
        Diagnostic.error( "C0006", "duplicate module `#{name}` in circuit manifest" )
          .with_primary( span, "redeclared here" )
          .with_secondary( first, "first declared here" )
      end

      def module_dir_not_found( name : String, path : String, span : Span ) : Diagnostic
        Diagnostic.error( "C0007", "module `#{name}` points to `#{path}` which is not a directory" )
          .with_primary( span, "no such directory relative to the project root" )
      end

      def path_escapes_project( path : String, span : Span ) : Diagnostic
        Diagnostic.error( "C0008", "path `#{path}` escapes the project root" )
          .with_primary( span, "manifest paths must be relative and stay inside the project" )
      end

      def link_unknown_module( name : String, span : Span, declared : Array( String ) ) : Diagnostic
        diag = Diagnostic.error( "C0009", "`@[Link(\"#{name}\")]` references a module not declared in the circuit manifest" )
          .with_primary( span, "unknown module `#{name}`" )
        if declared.empty?
          diag.with_help( "the manifest declares no modules; add a `modules( ... )` entry to `Project.vl`" )
        else
          diag.with_help( "declared modules are: #{declared.sort.join( ", " )}" )
        end
      end

      def circular_dependency( chain : Array( String ), span : Span ) : Diagnostic
        Diagnostic.error( "C0010", "circular module dependency: #{chain.join( " -> " )}" )
          .with_primary( span, "this `@[Link]` closes the cycle" )
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

      def deref_non_pointer( actual : String, span : Span ) : Diagnostic
        Diagnostic.error( "S0040", "cannot dereference non-pointer type `#{actual}`" )
          .with_primary( span )
      end

      def address_of_non_lvalue( span : Span ) : Diagnostic
        Diagnostic.error( "S0041", "cannot take address of non-lvalue expression" )
          .with_primary( span )
      end

      def pointer_escape( span : Span ) : Diagnostic
        Diagnostic.error( "S0042", "pointer to local variable escapes its scope" )
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

      def invalid_pipeline_target( span : Span ) : Diagnostic
        Diagnostic.error( "S0050", "invalid pipeline target: right-hand side of |> must be a function call, method call, or identifier" )
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

      def break_outside_loop( span : Span ) : Diagnostic
        Diagnostic.error( "S0052", "`break` used outside of a loop" )
          .with_primary( span )
      end

      def next_outside_loop( span : Span ) : Diagnostic
        Diagnostic.error( "S0053", "`next` used outside of a loop" )
          .with_primary( span )
      end

      def super_outside_method( span : Span ) : Diagnostic
        Diagnostic.error( "S0054", "`super` used outside of an instance method" )
          .with_primary( span )
      end

      def super_without_parent( owner : String, span : Span ) : Diagnostic
        Diagnostic.error( "S0055", "`super` used in `#{owner}`, which has no parent class" )
          .with_primary( span )
      end

      def super_no_parent_method( owner : String, name : String, span : Span ) : Diagnostic
        Diagnostic.error( "S0056", "no ancestor of `#{owner}` defines a method named `#{name}`" )
          .with_primary( span, "`super` called here" )
      end

      def generic_arity_mismatch( name : String, expected : Int32, got : Int32, span : Span ) : Diagnostic
        Diagnostic.error( "S0057", "`#{name}` expects #{expected} type argument#{expected == 1 ? "" : "s"}, got #{got}" )
          .with_primary( span )
      end

      def generic_needs_type_args( name : String, span : Span ) : Diagnostic
        Diagnostic.error( "S0058", "generic `#{name}` used without type arguments" )
          .with_primary( span, "write `#{name}[...]` or let the arguments be inferred from a constructor call" )
      end

      def cannot_infer_type_param( param : String, name : String, span : Span ) : Diagnostic
        Diagnostic.error( "S0059", "cannot infer type parameter `#{param}` of `#{name}` from this call" )
          .with_primary( span, "annotate the type arguments explicitly: `#{name}[...]`" )
      end

      def unknown_type_argument( name : String, span : Span ) : Diagnostic
        Diagnostic.error( "S0060", "unknown type `#{name}` used as a type argument" )
          .with_primary( span )
      end

      def instantiation_too_deep( mangled : String, span : Span ) : Diagnostic
        Diagnostic.error( "S0061", "generic instantiation `#{mangled}` nests deeper than #{Monomorphizer::MAX_NESTING} levels" )
          .with_primary( span, "likely an infinitely recursive generic type" )
      end

      def unsupported_in_template( what : String, span : Span ) : Diagnostic
        Diagnostic.error( "S0062", "`#{what}` is not supported inside a generic declaration" )
          .with_primary( span )
      end

      def generic_method_unsupported( name : String, span : Span ) : Diagnostic
        Diagnostic.error( "S0063", "generic method `#{name}` is not supported inside a type in #{VERSION}" )
          .with_primary( span, "only top-level generic functions and generic classes are supported" )
      end
    end


  end


end
