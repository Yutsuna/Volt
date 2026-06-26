module Volt::Frontend


  class Parser

    def initialize( source : String, file : String = "<unknown>" )
      @lexer       = Lexer.new( source, file )
      @file        = file
      @current     = @lexer.next_token
      @peek        = @lexer.next_token
      @paren_depth = 0
    end


    def parse : Program
      parse_program
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
        error!( "expected #{kind}, got `#{@current.value}`", @current.span )
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

    private def error!( msg : String, span : Span ) : NoReturn
      raise ParseError.new( msg, span )
    end

    #------------------------------------------------------------------------------------


    BODY_TERMINATORS = [ TokenKind::End, TokenKind::Else, TokenKind::Elsif,
                         TokenKind::When, TokenKind::Eof ]

    private def parse_body : Array( ANode )
      nodes = [] of ANode
      skip_separators
      until BODY_TERMINATORS.includes?( @current.kind )
        nodes << parse_body_node
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
          error!( "expected `def` after `async`", @current.span )
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
