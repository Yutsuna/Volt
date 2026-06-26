module Volt::Frontend


  class Parser

    private class ParseRecovery < Exception
    end


    getter bag : DiagnosticBag

    def initialize( source : String, file : String = "<unknown>" )
      @lexer       = Lexer.new( source, file )
      @file        = file
      @current     = @lexer.next_token
      @peek        = @lexer.next_token
      @paren_depth = 0
      @bag         = DiagnosticBag.new
    end


    def parse : Program
      program = parse_program
      raise CompilationError.new( @bag ) if @bag.errors?
      program
    end

    #------------------------------------------------------------------------------------

    private def advance : Token
      tok      = @current
      @current = @peek
      @peek    = @lexer.next_token
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
          nodes << parse_body_node
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
