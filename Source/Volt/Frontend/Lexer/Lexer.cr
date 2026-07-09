module Volt::Frontend


  #------------------------------------------------------------------------------------


  class Lexer

    KEYWORDS = {
      "def"       => TokenKind::Def,
      "end"       => TokenKind::End,
      "class"     => TokenKind::Class,
      "struct"    => TokenKind::Struct,
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
      "private"   => TokenKind::Private,
      "protected" => TokenKind::Protected,
      "public"    => TokenKind::Public,
      "extend"    => TokenKind::Extend,
      "circuit"   => TokenKind::Circuit,
      "true"      => TokenKind::True,
      "false"     => TokenKind::False,
      "nil"       => TokenKind::Nil,
      "macro"     => TokenKind::Macro,
      "__FILE__"  => TokenKind::DunderFile,
      "__LINE__"  => TokenKind::DunderLine,
      "__DIR__"   => TokenKind::DunderDir,
    }

    @src       : ::String
    @file      : ::String
    @ptr       : Pointer(UInt8)
    @buf_end   : Pointer(UInt8)
    @line      : UInt32
    @col       : UInt32
    @sptr      : Pointer(UInt8)
    @sln       : UInt32
    @scol      : UInt32
    @last_kind : TokenKind

    def initialize( source : ::String, file : ::String = "<unknown>" )
      @src       = source
      @file      = file
      @ptr       = source.to_unsafe
      @buf_end   = @ptr + source.bytesize
      @line      = 1_u32
      @col       = 1_u32
      @sptr      = @ptr
      @sln       = 1_u32
      @scol      = @col
      @last_kind = TokenKind::Newline
    end


    def next_token : Token
      tok = scan_next
      @last_kind = tok.kind
      tok
    end

    # Eagerly lex `source` into a token array terminated by a single `Eof`. Used to
    # re-lex macro-expanded source before re-parsing it.
    def self.tokenize( source : ::String, file : ::String ) : Array( Token )
      lexer  = new( source, file )
      tokens = [] of Token
      loop do
        tok = lexer.next_token
        tokens << tok
        break if tok.kind.eof?
      end
      tokens
    end

    #------------------------------------------------------------------------------------

    private def scan_next : Token
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
      when '+'
        if !at_end? && cur == '='.ord.to_u8
          step; make( TokenKind::PlusEq )
        else
          make( TokenKind::Plus )
        end
      when '-'
        if !at_end?
          case cur
          when '>'.ord.to_u8
            step; make( TokenKind::Arrow )
          when '='.ord.to_u8
            step; make( TokenKind::MinusEq )
          else
            make( TokenKind::Minus )
          end
        else
          make( TokenKind::Minus )
        end
      when '*'
        if !at_end?
          case cur
          when '*'.ord.to_u8
            step; make( TokenKind::StarStar )
          when '='.ord.to_u8
            step; make( TokenKind::StarEq )
          else
            make( TokenKind::Star )
          end
        else
          make( TokenKind::Star )
        end
      when '/'
        if division_allowed?
          if !at_end? && cur == '/'.ord.to_u8
            step
            if !at_end? && cur == '='.ord.to_u8
              step; make( TokenKind::SlashSlashEq )
            else
              make( TokenKind::SlashSlash )
            end
          elsif !at_end? && cur == '='.ord.to_u8
            step; make( TokenKind::SlashEq )
          else
            make( TokenKind::Slash )
          end
        else
          scan_regex
        end
      when '%'
        if !at_end? && cur == '='.ord.to_u8
          step; make( TokenKind::PercentEq )
        elsif !at_end? && cur == '}'.ord.to_u8
          step; make( TokenKind::RMacroExpr )
        else
          make( TokenKind::Percent )
        end
      when '='
        if !at_end?
          case cur
          when '='.ord.to_u8
            step
            if !at_end? && cur == '='.ord.to_u8
              step; make( TokenKind::EqEqEq )
            else
              make( TokenKind::EqEq )
            end
          when '~'.ord.to_u8
            step; make( TokenKind::MatchOp )
          when '>'.ord.to_u8
            step; make( TokenKind::FatArrow )
          else
            make( TokenKind::Eq )
          end
        else
          make( TokenKind::Eq )
        end
      when '!'
        if !at_end?
          case cur
          when '='.ord.to_u8
            step; make( TokenKind::BangEq )
          when '~'.ord.to_u8
            step; make( TokenKind::NotMatchOp )
          else
            make( TokenKind::Bang )
          end
        else
          make( TokenKind::Bang )
        end
      when '<'
        if !at_end?
          case cur
          when '<'.ord.to_u8
            step; make( TokenKind::LtLt )
          when '='.ord.to_u8
            step
            if !at_end? && cur == '>'.ord.to_u8
              step; make( TokenKind::Spaceship )
            else
              make( TokenKind::LtEq )
            end
          else
            make( TokenKind::Lt )
          end
        else
          make( TokenKind::Lt )
        end
      when '>'
        if !at_end?
          case cur
          when '>'.ord.to_u8
            step; make( TokenKind::GtGt )
          when '='.ord.to_u8
            step; make( TokenKind::GtEq )
          else
            make( TokenKind::Gt )
          end
        else
          make( TokenKind::Gt )
        end
      when '&'
        if !at_end?
          case cur
          when '&'.ord.to_u8
            step; make( TokenKind::AmpAmp )
          when '+'.ord.to_u8
            step
            if !at_end? && cur == '='.ord.to_u8
              step; make( TokenKind::AmpPlusEq )
            else
              make( TokenKind::AmpPlus )
            end
          when '-'.ord.to_u8
            step; make( TokenKind::AmpMinus )
          when '*'.ord.to_u8
            step
            if !at_end? && cur == '*'.ord.to_u8
              step; make( TokenKind::AmpStarStar )
            else
              make( TokenKind::AmpStar )
            end
          when '='.ord.to_u8
            step; make( TokenKind::AmpEq )
          else
            make( TokenKind::Amp )
          end
        else
          make( TokenKind::Amp )
        end
      when '|'
        if !at_end?
          case cur
          when '|'.ord.to_u8
            step; make( TokenKind::PipePipe )
          when '='.ord.to_u8
            step; make( TokenKind::PipeEq )
          when '>'.ord.to_u8
            step; make( TokenKind::PipeGt )
          else
            make( TokenKind::Pipe )
          end
        else
          make( TokenKind::Pipe )
        end
      when '^'
        if !at_end? && cur == '='.ord.to_u8
          step; make( TokenKind::CaretEq )
        else
          make( TokenKind::Caret )
        end
      when '~'  then make( TokenKind::Tilde )
      when '.'  then scan_dot
      when '?'  then make( TokenKind::Question )
      when '@'
        if !at_end? && cur == '@'.ord.to_u8
          step
          scan_ident_chars
          make( TokenKind::ClassVar )
        else
          make( TokenKind::At )
        end
      when ':'
        if !at_end? && cur == ':'.ord.to_u8
          step; make( TokenKind::ColonColon )
        else
          make( TokenKind::Colon )
        end
      when ','  then make( TokenKind::Comma )
      when ';'  then make( TokenKind::Semicolon )
      when '('  then make( TokenKind::LParen )
      when ')'  then make( TokenKind::RParen )
      when '['  then make( TokenKind::LBracket )
      when ']'  then make( TokenKind::RBracket )
      when '{'
        if !at_end? && cur == '{'.ord.to_u8
          step; make( TokenKind::LDoubleBrace )
        elsif !at_end? && cur == '%'.ord.to_u8
          step; make( TokenKind::LMacroExpr )
        else
          make( TokenKind::LBrace )
        end
      when '}'
        if !at_end? && cur == '}'.ord.to_u8
          step; make( TokenKind::RDoubleBrace )
        else
          make( TokenKind::RBrace )
        end
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
      off = ( @sptr - @src.to_unsafe ).to_u32
      Token.new(
        kind: kind,
        ptr:  @sptr,
        len:  len,
        span: Span.new( file: @file, line: @sln, column: @scol, length: len.to_u32, offset: off )
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

    private def scan_dot : Token
      return make( TokenKind::Dot ) if at_end? || cur != '.'.ord.to_u8
      step
      return make( TokenKind::DotDotDot ) if !at_end? && cur == '.'.ord.to_u8 && ( step; true )
      make( TokenKind::DotDot )
    end

    private def scan_ident_or_keyword : Token
      scan_ident_chars
      len  = ( @ptr - @sptr ).to_i32
      text = ::String.new( @sptr, len )
      kind = KEYWORDS[ text ]? || TokenKind::Ident
      make( kind )
    end

    # Advances `@ptr` over a run of identifier characters (no token produced).
    # Shared by the plain identifier scanner and the `@@class_var` scanner.
    private def scan_ident_chars : Nil
      while !at_end?
        c = cur.unsafe_chr
        break unless c.alphanumeric? || cur == '_'.ord || cur == '?'.ord || cur == '!'.ord
        step
      end
    end

    private def scan_number : Token
      is_float = false
      while !at_end?
        c = cur
        if c.unsafe_chr.ascii_number?
          step
        elsif c == '_'.ord
          nxt = peek_next
          if nxt == 'i'.ord || nxt == 'u'.ord
            step # consume '_'
            step # consume 'i' or 'u'
            while !at_end? && cur.unsafe_chr.ascii_number?
              step
            end
            break
          else
            step
          end
        elsif c == '.'.ord && peek_next.unsafe_chr.ascii_number?
          is_float = true
          step
          while !at_end? && ( cur.unsafe_chr.ascii_number? || cur == '_'.ord )
            step
          end
        elsif c == 'e'.ord || c == 'E'.ord
          is_float = true
          step
          if !at_end? && ( cur == '+'.ord || cur == '-'.ord )
            step
          end
          while !at_end? && ( cur.unsafe_chr.ascii_number? || cur == '_'.ord )
            step
          end
        else
          break
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

    private def scan_regex : Token
      while !at_end?
        if cur == '\\'.ord.to_u8
          step
          step
        elsif cur == '/'.ord.to_u8
          step
          return make( TokenKind::Regex )
        elsif cur == '\n'.ord.to_u8
          return make( TokenKind::Error )
        else
          step
        end
      end
      make( TokenKind::Error )
    end

    private def division_allowed? : Bool
      case @last_kind
      # `TokenKind::Nil` spelled as a constant: `.nil?` would call
      # `Object#nil?` (always false), silently dropping `nil` from the set.
      when .int?, .float?, .string?, .true?, .false?, TokenKind::Nil, .ident?, .self_?, .super?, .r_paren?, .r_bracket?, .r_brace?
        true
      when .def?   # `def /` declares the division operator, never a regex
        true
      else
        false
      end
    end


  end


end
