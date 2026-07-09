module Volt::Frontend


  #------------------------------------------------------------------------------------


  struct Span
    property file   : String
    property line   : UInt32
    property column : UInt32
    property length : UInt32
    property offset : UInt32

    def initialize( @file : String, @line : UInt32, @column : UInt32, @length : UInt32, @offset : UInt32 = 0_u32 )
    end
  end


  #------------------------------------------------------------------------------------



  enum TokenKind
    # literals
    Int
    Float
    String
    True
    False
    Nil

    # identifier (reclassified to a keyword below if matched)
    Ident

    # keywords
    Def
    End
    Class
    Struct
    Mixin
    Component
    Include
    Use
    Module
    If
    Unless
    Else
    Elsif
    Then
    When
    Match
    Do
    While
    Until
    For
    Break
    Next
    Return
    And
    Or
    Not
    In
    Is
    As
    Async
    Await
    Self_
    Super
    Raise
    Abstract
    Macro
    Private
    Protected
    Public
    Extend
    Typeof

    # arithmetic
    Plus        # +
    Minus       # -
    Star        # *
    Slash       # /
    Percent     # %
    StarStar    # **
    AmpPlus     # &+
    AmpMinus    # &-
    AmpStar     # &*
    AmpStarStar # &**
    SlashSlash  # //

    # equality / comparison
    EqEq        # ==
    BangEq      # !=
    Lt          # <
    Gt          # >
    LtEq        # <=
    GtEq        # >=
    Spaceship   # <=>
    EqEqEq      # ===
    MatchOp     # =~
    NotMatchOp  # !~

    # logical
    AmpAmp      # &&
    PipePipe    # ||
    Bang        # !

    # assignment
    Eq          # =
    PlusEq        # +=
    MinusEq       # -=
    StarEq        # *=
    SlashEq       # /=
    SlashSlashEq  # //=
    PercentEq     # %=
    PipeEq        # |=
    AmpEq         # &=
    CaretEq       # ^=
    AmpPlusEq     # &+=

    # structural / punctuation
    PipeGt      # |>
    Pipe        # |
    Dot         # .
    DotDot      # ..
    DotDotDot   # ...
    Arrow       # ->
    SafeNav     # ?.
    Colon       # :
    ColonColon  # ::
    Comma       # ,
    Semicolon   # ;
    At          # @
    ClassVar    # @@name  (module/class variable; value holds the bare name)
    Question    # ?
    Hash        # #
    Amp         # &
    Caret       # ^
    Tilde       # ~
    LtLt        # <<
    GtGt        # >>

    # delimiters
    LParen      # (
    RParen      # )
    LBracket    # [
    RBracket    # ]
    LBrace      # {
    RBrace      # }
    LDoubleBrace # {{
    RDoubleBrace # }}
    LMacroExpr   # {%
    RMacroExpr   # %}

    # pseudo-variables (handled as keywords)
    DunderFile  # __FILE__
    DunderLine  # __LINE__
    DunderDir   # __DIR__

    # regex literal
    Regex

    # whitespace / meta
    Newline
    Eof
    Error
  end


  #------------------------------------------------------------------------------------


  struct Token
    property kind : TokenKind
    property ptr  : Pointer(UInt8)
    property len  : Int32
    property span : Span

    def initialize( @kind : TokenKind, @ptr : Pointer(UInt8), @len : Int32, @span : Span )
    end

    def value : ::String
      ::String.new( ptr, len )
    end

    def value_eq?( s : ::String ) : Bool
      return false unless len == s.bytesize
      ptr.memcmp( s.to_unsafe, len ) == 0
    end
  end


end
