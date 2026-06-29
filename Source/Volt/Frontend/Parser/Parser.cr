module Volt::Frontend


  struct MacroDef
    property name   : String
    property params : Array( String )
    property body   : Array( Token )

    def initialize( @name, @params, @body )
    end
  end


  class Parser

    private class ParseRecovery < Exception
    end


    getter bag : DiagnosticBag

    @tokens_stream   : Array( Token )? = nil
    @token_index     : Int32 = 0
    @macro_expander  : MacroParser? = nil
    @macro_depth     : Int32 = 0

    # TODO: make this parametrable via env or cli or idk
    # this is a guard against recursive macro expansion
    MAX_MACRO_DEPTH = 256

    def initialize( source : String, file : String = "<unknown>" )
      @lexer       = Lexer.new( source, file )
      @file        = file
      @current     = @lexer.next_token
      @peek        = @lexer.next_token
      @paren_depth = 0
      @bag         = DiagnosticBag.new
      @macro_table = {} of String => MacroDef
    end

    def initialize( tokens : Array( Token ), @file : String, @bag : DiagnosticBag )
      @lexer         = Lexer.new( "", @file )
      @tokens_stream = tokens
      @current       = tokens[ 0 ]? || Token.new( TokenKind::Eof, Pointer( UInt8 ).null, 0, Span.new( @file, 1, 1, 0 ) )
      @peek          = tokens[ 1 ]? || Token.new( TokenKind::Eof, Pointer( UInt8 ).null, 0, Span.new( @file, 1, 1, 0 ) )
      @token_index   = 2
      @paren_depth   = 0
      @macro_table   = {} of String => MacroDef
    end


    def parse : Program
      program = parse_program
      raise CompilationError.new( @bag ) if @bag.errors?
      program
    end

    #------------------------------------------------------------------------------------

    private def macro_expander : MacroParser
      @macro_expander ||= MacroParser.new( @file )
    end

    protected def import_macros( table : Hash( String, MacroDef ), depth : Int32 ) : Nil
      @macro_table = table
      @macro_depth = depth
    end

    protected def parse_expansion_nodes : Array( ANode )
      nodes = [] of ANode
      collect_nodes( nodes )
      nodes
    end

    protected def parse_expansion_expr : AExpr
      skip_separators
      parse_expr
    end

    #------------------------------------------------------------------------------------

    private def advance : Token
      tok = @current
      if stream = @tokens_stream
        @current = @peek
        @peek = stream[ @token_index ]? || Token.new( TokenKind::Eof, Pointer( UInt8 ).null, 0, tok.span )
        @token_index += 1
      else
        @current = @peek
        @peek    = @lexer.next_token
      end
      tok
    end

    private def check( kind : TokenKind ) : Bool
      skip_newlines if @paren_depth > 0
      @current.kind == kind
    end

    private def expect( kind : TokenKind ) : Token
      skip_newlines if @paren_depth > 0
      unless @current.kind == kind
        error!( Catalog::Parse.expected( kind, @current ) )
      end
      advance
    end

    private def expect_close( kind : TokenKind, opener : Span, opener_desc : String ) : Token
      skip_newlines if @paren_depth > 0
      unless @current.kind == kind
        error!( Catalog::Parse.unclosed( kind, @current, opener, opener_desc ) )
      end
      advance
    end

    private def skip_newlines
      while @current.kind.newline?
        advance
      end
    end

    private def skip_separators
      while @current.kind.newline? || @current.kind.semicolon?
        advance
      end
    end

    private def at_end? : Bool
      @current.kind.eof?
    end

    private def error!( diagnostic : Diagnostic ) : NoReturn
      @bag << diagnostic
      raise ParseRecovery.new
    end

    private def synchronize : Nil
      @paren_depth = 0
      return if at_end?
      advance
      until at_end?
        return if @current.kind.newline? || @current.kind.semicolon?
        return if BODY_TERMINATORS.includes?( @current.kind )
        return if statement_start?( @current.kind )
        advance
      end
    end

    private def statement_start?( kind : TokenKind ) : Bool
      case kind
      when .def?, .class?, .mixin?, .component?, .use?, .async?,
           .if?, .unless?, .while?, .until?, .match?, .return?, .break?, .next?
        true
      else
        false
      end
    end

    #------------------------------------------------------------------------------------


    BODY_TERMINATORS = [ TokenKind::End, TokenKind::Else, TokenKind::Elsif,
                         TokenKind::When, TokenKind::Eof ]

    private def parse_body : Array( ANode )
      nodes = [] of ANode
      skip_separators
      until BODY_TERMINATORS.includes?( @current.kind )
        begin
          if macro_invocation?
            nodes.concat( expand_macro_statements )
          else
            nodes << parse_body_node
          end
        rescue ParseRecovery
          synchronize
        end
        skip_separators
      end
      nodes
    end

    private def parse_body_node : ANode
      annots = collect_annotations
      case @current.kind
      when .def?
        parse_func_decl( annots, is_async: false )
      when .async?
        advance
        if @current.kind.def?
          parse_func_decl( annots, is_async: true )
        else
          error!( Catalog::Parse.expected_after( "`def`", "async", @current ) )
        end
      when .class?
        parse_class_decl( annots )
      when .mixin?
        parse_mixin_decl
      else
        parse_expr_node
      end
    end

    private def parse_expr_node : AExpr
      parse_expr
    end

    #------------------------------------------------------------------------------------


  end


end
