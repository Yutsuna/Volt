module Volt
  module Lexer


    enum EToken
      # Literals
      Integer
      Float
      Char
      Str

      # Identifiers & keywords
      Identifier
      KwDef
      KwEnd
      KwIf
      KwElsif
      KwElse
      KwUnless
      KwWhile
      KwUntil
      KwDo
      KwReturn
      KwTrue
      KwFalse
      KwNil
      KwTypeof
      KwPointerof

      # Operators
      Plus
      Minus
      Star
      Slash
      Percent
      EqEq
      NotEq
      Lt
      Gt
      Le
      Ge
      AndAnd
      OrOr
      Not
      Amp
      Pipe
      Caret
      Tilde
      Shl
      Shr
      Assign
      PlusAssign
      MinusAssign
      Question
      Colon

      # Punctuation
      LParen
      RParen
      LBracket
      RBracket
      Comma
      AnnotationStart   # @[

      # Structural
      Newline
      EOF
    end


  end
end
