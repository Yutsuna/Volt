module Volt
  module Parser


  class FParser

    #--------------------------------------------------------------------------
    # Top level declaration parsers
    #--------------------------------------------------------------------------

    private def parse_extern : Ast::ExternDef
      expect(Lexer::EToken::AnnotationStart)
      expect(Lexer::EToken::Identifier) # "Extern"
      expect(Lexer::EToken::RBracket)
      skip_newlines
      expect(Lexer::EToken::KwDef)
      name   = expect(Lexer::EToken::Identifier).lexeme
      params = parse_params
      ret    = parse_optional_return_type
      Ast::ExternDef.new(name, params, ret)
    end

    private def parse_def : Ast::Def
      expect(Lexer::EToken::KwDef)
      name   = expect(Lexer::EToken::Identifier).lexeme
      params = parse_params
      ret    = parse_optional_return_type
      body   = parse_block([Lexer::EToken::KwEnd])
      expect(Lexer::EToken::KwEnd)
      Ast::Def.new(name, params, ret, body)
    end

    private def parse_params : Array(Ast::Param)
      params = [] of Ast::Param
      return params unless accept(Lexer::EToken::LParen)
      unless check(Lexer::EToken::RParen)
        loop do
          pname = expect(Lexer::EToken::Identifier).lexeme
          expect(Lexer::EToken::Colon)
          params << Ast::Param.new(pname, parse_type)
          break unless accept(Lexer::EToken::Comma)
        end
      end
      expect(Lexer::EToken::RParen)
      params
    end

    private def parse_optional_return_type : Types::Type
      return Types::Type.new(Types::EType::Void) unless accept(Lexer::EToken::Colon)
      parse_type
    end

    private def parse_type : Types::Type
      name = expect(Lexer::EToken::Identifier).lexeme
      base = Types::Type.named(name)
      unless base
        @reporter.error("unknown type '#{name}'", peek.line, peek.col)
        base = Types::Type.new(Types::EType::Int32)
      end
      depth = 0
      while accept(Lexer::EToken::Star)
        depth += 1
      end
      Types::Type.new(base.base, depth)
    end

    #--------------------------------------------------------------------------

  end


  end
end
