module Volt::Frontend


  #-----------------------------------------------------------------------------------

  class ParseError < Exception
    getter span : Span

    def initialize( message : ::String, @span : Span )
      super( message )
    end
  end

  #------------------------------------------------------------------------------------


  class Lexer

    KEYWORDS = {
      "def"       => TokenKind::Def,
      "end"       => TokenKind::End,
      "class"     => TokenKind::Class,
      "mixin"     => TokenKind::Mixin,
      "component" => TokenKind::Component,
      "include"   => TokenKind::Include,
      "use"       => TokenKind::Use,
      "module"    => TokenKind::Module,
      "if"        => TokenKind::If,
      "unless"    => TokenKind::Unless,
      "else"      => TokenKind::Else,
      "elsif"     => TokenKind::Elsif,
      "then"      => TokenKind::Then,
      "when"      => TokenKind::When,
      "match"     => TokenKind::Match,
      "do"        => TokenKind::Do,
      "while"     => TokenKind::While,
      "until"     => TokenKind::Until,
      "for"       => TokenKind::For,
      "break"     => TokenKind::Break,
      "next"      => TokenKind::Next,
      "return"    => TokenKind::Return,
      "and"       => TokenKind::And,
      "or"        => TokenKind::Or,
      "not"       => TokenKind::Not,
      "in"        => TokenKind::In,
      "is"        => TokenKind::Is,
      "as"        => TokenKind::As,
      "async"     => TokenKind::Async,
      "await"     => TokenKind::Await,
      "self"      => TokenKind::Self_,
      "super"     => TokenKind::Super,
      "raise"     => TokenKind::Raise,
      "abstract"  => TokenKind::Abstract,
      "true"      => TokenKind::True,
      "false"     => TokenKind::False,
      "nil"       => TokenKind::Nil,
    }

    @src     : ::String
    @file    : ::String
    @ptr     : Pointer(UInt8)
    @buf_end : Pointer(UInt8)
    @line    : UInt32
    @col     : UInt32
    @sptr    : Pointer(UInt8)
    @sln     : UInt32
    @scol    : UInt32

    def initialize( source : ::String, file : ::String = "<unknown>" )
      @src     = source
      @file    = file
      @ptr     = source.to_unsafe
      @buf_end = @ptr + source.bytesize
      @line    = 1_u32
      @col     = 1_u32
      @sptr    = @ptr
      @sln     = 1_u32
      @scol    = 1_u32
    end


    def next_token : Token
      skip_whitespace
      skip_line_comment if !at_end? && cur == '#'.ord.to_u8

      mark_start
      return make( TokenKind::Eof ) if at_end?

      c = cur
      step

      case c.unsafe_chr
      when '\n'
        nl = make( TokenKind::Newline )
        @line += 1
        @col = 1
        nl
      when '+'  then make( TokenKind::Plus )
      when '-'
        if !at_end? && cur == '>'.ord.to_u8
          step; make( TokenKind::Arrow )
        else
          make( TokenKind::Minus )
        end
      when '*'
        if !at_end? && cur == '*'.ord.to_u8
          step; make( TokenKind::StarStar )
        else
          make( TokenKind::Star )
        end
      when '/'  then make( TokenKind::Slash )
      when '%'  then make( TokenKind::Percent )
      when '='
        if !at_end? && cur == '='.ord.to_u8
          step; make( TokenKind::EqEq )
        else
          make( TokenKind::Eq )
        end
      when '!'
        if !at_end? && cur == '='.ord.to_u8
          step; make( TokenKind::BangEq )
        else
          make( TokenKind::Bang )
        end
      when '<'
        if !at_end? && cur == '='.ord.to_u8
          step; make( TokenKind::LtEq )
        else
          make( TokenKind::Lt )
        end
      when '>'
        if !at_end? && cur == '='.ord.to_u8
          step; make( TokenKind::GtEq )
        else
          make( TokenKind::Gt )
        end
      when '&'
        if !at_end? && cur == '&'.ord.to_u8
          step; make( TokenKind::AmpAmp )
        else
          make( TokenKind::Error )
        end
      when '|'  then scan_pipe
      when '.'  then scan_dot
      when '?'  then make( TokenKind::Question )
      when '@'  then make( TokenKind::At )
      when ':'  then make( TokenKind::Colon )
      when ','  then make( TokenKind::Comma )
      when ';'  then make( TokenKind::Semicolon )
      when '('  then make( TokenKind::LParen )
      when ')'  then make( TokenKind::RParen )
      when '['  then make( TokenKind::LBracket )
      when ']'  then make( TokenKind::RBracket )
      when '{'  then make( TokenKind::LBrace )
      when '}'  then make( TokenKind::RBrace )
      when '"', '\''
        scan_string( c.unsafe_chr )
      when '0'..'9'
        scan_number
      when 'a'..'z', 'A'..'Z', '_'
        scan_ident_or_keyword
      else
        make( TokenKind::Error )
      end
    end

    #------------------------------------------------------------------------------------

    private def cur : UInt8
      @ptr.value
    end

    private def peek_next : UInt8
      return 0_u8 if @ptr + 1 >= @buf_end
      ( @ptr + 1 ).value
    end

    private def step
      @col += 1
      @ptr += 1
    end

    private def at_end? : Bool
      @ptr >= @buf_end
    end

    private def mark_start
      @sptr = @ptr
      @sln  = @line
      @scol = @col
    end

    private def make( kind : TokenKind ) : Token
      len = ( @ptr - @sptr ).to_i32
      Token.new(
        kind: kind,
        ptr:  @sptr,
        len:  len,
        span: Span.new( file: @file, line: @sln, column: @scol, length: len.to_u32 )
      )
    end

    private def skip_whitespace
      while !at_end? && ( cur == ' '.ord || cur == '\t'.ord || cur == '\r'.ord )
        step
      end
    end

    private def skip_line_comment
      while !at_end? && cur != '\n'.ord
        step
      end
    end

    private def scan_pipe : Token
      return make( TokenKind::PipePipe ) if !at_end? && cur == '|'.ord.to_u8 && ( step; true )
      return make( TokenKind::PipeGt )   if !at_end? && cur == '>'.ord.to_u8 && ( step; true )
      make( TokenKind::Pipe )
    end

    private def scan_dot : Token
      return make( TokenKind::Dot ) if at_end? || cur != '.'.ord.to_u8
      step
      return make( TokenKind::DotDotDot ) if !at_end? && cur == '.'.ord.to_u8 && ( step; true )
      make( TokenKind::DotDot )
    end

    private def scan_ident_or_keyword : Token
      while !at_end?
        c = cur.unsafe_chr
        break unless c.alphanumeric? || cur == '_'.ord || cur == '?'.ord || cur == '!'.ord
        step
      end
      len  = ( @ptr - @sptr ).to_i32
      text = ::String.new( @sptr, len )
      kind = KEYWORDS[ text ]? || TokenKind::Ident
      make( kind )
    end

    private def scan_number : Token
      is_float = false
      while !at_end? && cur.unsafe_chr.ascii_number?
        step
      end
      if !at_end? && cur == '.'.ord.to_u8 && peek_next.unsafe_chr.ascii_number?
        is_float = true
        step
        while !at_end? && cur.unsafe_chr.ascii_number?
          step
        end
      end
      if !at_end? && ( cur == 'e'.ord || cur == 'E'.ord )
        is_float = true
        step
        step if !at_end? && ( cur == '+'.ord || cur == '-'.ord )
        while !at_end? && cur.unsafe_chr.ascii_number?
          step
        end
      end
      make( is_float ? TokenKind::Float : TokenKind::Int )
    end

    private def scan_string( quote : Char ) : Token
      while !at_end? && cur.unsafe_chr != quote
        if cur == '\\'.ord.to_u8
          step
          step
        elsif cur == '#'.ord.to_u8 && peek_next == '{'.ord.to_u8
          step; step
        else
          step
        end
      end
      step unless at_end?
      make( TokenKind::String )
    end


  end


end
