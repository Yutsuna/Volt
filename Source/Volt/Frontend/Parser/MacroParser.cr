module Volt::Frontend


  # Compile-time macro expansion engine.
  #
  # Supported today:
  #   * `{{ expr }}` interpolation, with macro-parameter substitution and a small
  #     set of compile-time methods (`id`, `stringify`, `upcase`, `downcase`,
  #     `camelcase`).
  #   * `{% if cond %} … {% elsif %} … {% else %} … {% end %}` conditionals.
  #   * `{% for x in [a, b, …] %} … {% end %}` loops (unrolled at expansion time).
  #   * The `__FILE__`, `__LINE__` and `__DIR__` pseudo-variables, which resolve to
  #     the *call site* — exactly like Crystal.
  class MacroParser

    class ExpansionError < Exception
      getter span : Span

      def initialize( message : String, @span : Span )
        super( message )
      end
    end


    @cursor : UInt32? = nil


    def initialize( @file : String )
    end


    def expand( macro_def : MacroDef, args : Array( Array( Token ) ), call_span : Span ) : String
      bindings = bind_parameters( macro_def, args )
      @cursor  = nil
      String.build do |io|
        emit_sequence( io, macro_def.body, 0, macro_def.body.size, bindings, call_span )
      end
    end

    #------------------------------------------------------------------------------------


    # Find the index of the token that closes the bracket opened at `open`, scanning the
    # matching `{{`/`{%` nesting so inner braces don't confuse us.
    private def matching( body : Array( Token ), open : Int32, to : Int32,
                          close_kind : TokenKind, opener : String ) : Int32
      open_kind = body[ open ].kind
      depth     = 1
      i         = open + 1
      while i < to
        k = body[ i ].kind
        if k == open_kind
          depth += 1
        elsif k == close_kind
          depth -= 1
          return i if depth == 0
        end
        i += 1
      end
      raise ExpansionError.new( "unterminated `#{ opener }`", body[ open ].span )
    end


    private def reconstruct( tokens : Array( Token ) ) : String
      String.build do |io|
        tokens.each_with_index do |tok, idx|
          io << ' ' if idx > 0
          io << ( tok.kind.newline? ? " " : tok.value )
        end
      end.strip
    end


    private def reconstruct_with_bindings( tokens : Array( Token ),
                                           bindings : Hash( String, String ) ) : String
      String.build do |io|
        tokens.each_with_index do |tok, idx|
          io << ' ' if idx > 0
          if tok.kind.ident? && ( val = bindings[ tok.value ]? )
            io << val
          else
            io << tok.value
          end
        end
      end.strip
    end


    private def strip_newlines( tokens : Array( Token ) ) : Array( Token )
      tokens.reject( &.kind.newline? )
    end


    private def quote( s : String ) : String
      String.build do |io|
        io << '"'
        s.each_char do |c|
          case c
          when '"'  then io << "\\\""
          when '\\' then io << "\\\\"
          else           io << c
          end
        end
        io << '"'
      end
    end


    # Remove surrounding quotes from a string literal, leaving other text untouched.
    private def unquote( s : String ) : String
      t = s.strip
      if t.size >= 2 && ( ( t[ 0 ] == '"' && t[ -1 ] == '"' ) || ( t[ 0 ] == '\'' && t[ -1 ] == '\'' ) )
        t[ 1...-1 ]
      else
        t
      end
    end


    private def directory_of( file : String ) : String
      idx = file.rindex( '/' )
      idx ? file[ 0...idx ] : "."
    end


  end


end
