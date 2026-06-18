module Volt
  module Parser


    module EPrecedence

      extend self

      def binding_power ( kind : Lexer::EToken ) : Int32
        case kind
        when Lexer::EToken::OrOr                                                        then  1
        when Lexer::EToken::AndAnd                                                      then  2
        when Lexer::EToken::Pipe                                                        then  3
        when Lexer::EToken::Caret                                                       then  4
        when Lexer::EToken::Amp                                                         then  5
        when Lexer::EToken::EqEq, Lexer::EToken::NotEq                                  then  6
        when Lexer::EToken::Lt, Lexer::EToken::Gt, Lexer::EToken::Le, Lexer::EToken::Ge then  7
        when Lexer::EToken::Shl, Lexer::EToken::Shr                                     then  8
        when Lexer::EToken::Plus, Lexer::EToken::Minus                                  then  9
        when Lexer::EToken::Star, Lexer::EToken::Slash, Lexer::EToken::Percent          then  10
        else                                                                                  0
        end
      end

      def symbol ( kind : Lexer::EToken ) : String
        case kind
        when Lexer::EToken::OrOr    then "||"
        when Lexer::EToken::AndAnd  then "&&"
        when Lexer::EToken::Pipe    then "|"
        when Lexer::EToken::Caret   then "^"
        when Lexer::EToken::Amp     then "&"
        when Lexer::EToken::EqEq    then "=="
        when Lexer::EToken::NotEq   then "!="
        when Lexer::EToken::Lt      then "<"
        when Lexer::EToken::Gt      then ">"
        when Lexer::EToken::Le      then "<="
        when Lexer::EToken::Ge      then ">="
        when Lexer::EToken::Shl     then "<<"
        when Lexer::EToken::Shr     then ">>"
        when Lexer::EToken::Plus    then "+"
        when Lexer::EToken::Minus   then "-"
        when Lexer::EToken::Star    then "*"
        when Lexer::EToken::Slash   then "/"
        when Lexer::EToken::Percent then "%"
        else                             "?"
        end
      end

    end


  end
end
