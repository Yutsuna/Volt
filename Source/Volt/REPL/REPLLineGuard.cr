module Volt::REPL


  class REPLLineGuard

    OPEN_KEYWORDS = {
      Frontend::TokenKind::Def, Frontend::TokenKind::Class, Frontend::TokenKind::Struct,
      Frontend::TokenKind::Mixin, Frontend::TokenKind::Module, Frontend::TokenKind::If,
      Frontend::TokenKind::Unless, Frontend::TokenKind::While, Frontend::TokenKind::Until,
      Frontend::TokenKind::Match
    }

    def self.incomplete?( source : String ) : Bool
      lexer = Frontend::Lexer.new source
      indent_level = 0
      parentheses = 0
      brackets = 0
      braces = 0

      loop do
        tok = lexer.next_token
        break if tok.kind.eof?

        case tok.kind
        when .l_paren?   then parentheses += 1
        when .r_paren?   then parentheses -= 1
        when .l_bracket? then brackets += 1
        when .r_bracket? then brackets -= 1
        when .l_brace?   then braces += 1
        when .r_brace?   then braces -= 1
        when .end?       then indent_level -= 1
        else
          indent_level += 1 if OPEN_KEYWORDS.includes?(tok.kind)
        end
      end

      indent_level > 0 || parentheses > 0 || brackets > 0 || braces > 0
    rescue
      false
    end

  end


end
