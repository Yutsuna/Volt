module Volt
  module Lexer


    class FLexer

      KEYWORDS = {
        "def"       => EToken::KwDef,
        "end"       => EToken::KwEnd,
        "if"        => EToken::KwIf,
        "elsif"     => EToken::KwElsif,
        "else"      => EToken::KwElse,
        "unless"    => EToken::KwUnless,
        "while"     => EToken::KwWhile,
        "until"     => EToken::KwUntil,
        "do"        => EToken::KwDo,
        "return"    => EToken::KwReturn,
        "true"      => EToken::KwTrue,
        "false"     => EToken::KwFalse,
        "nil"       => EToken::KwNil,
        "typeof"    => EToken::KwTypeof,
        "pointerof" => EToken::KwPointerof,
      }

      CONTINUATION = [
        EToken::Plus, EToken::Minus, EToken::Star, EToken::Slash, EToken::Percent,
        EToken::EqEq, EToken::NotEq, EToken::Lt, EToken::Gt, EToken::Le, EToken::Ge,
        EToken::AndAnd, EToken::OrOr, EToken::Amp, EToken::Pipe, EToken::Caret,
        EToken::Shl, EToken::Shr, EToken::Assign, EToken::PlusAssign,
        EToken::MinusAssign, EToken::Question, EToken::Colon, EToken::Comma,
        EToken::LParen, EToken::LBracket, EToken::KwDo, EToken::AnnotationStart,
      ]

      #--------------------------------------------------------------------------

      def initialize ( @src : String, @reporter : Diagnostic::FReporter )
        @tokens = [] of Token
        @pos    = 0
        @line   = 1
        @col    = 1
      end

      #--------------------------------------------------------------------------

      def scan : Array(Token)
        until eof?
          scan_token
        end
        emit_newline_terminator
        push(EToken::EOF, "")
        @tokens
      end

      #--------------------------------------------------------------------------

      private def scan_token : Nil
        c = current

        case
        when c == '\n'
          consume_newline
        when c == ' ' || c == '\t' || c == '\r'
          advance
        when c == '#'
          skip_comment
        when c.ascii_number?
          scan_number
        when c == '_' || c.ascii_letter?
          scan_identifier
        when c == '"'
          scan_string
        when c == '\''
          scan_char
        else
          scan_operator
        end
      end

      #--------------------------------------------------------------------------

      private def consume_newline : Nil
        emit_newline_terminator
        advance_line
      end

      private def emit_newline_terminator : Nil
        last = @tokens.last?
        return if last.nil?
        return if last.kind == EToken::Newline
        return if CONTINUATION.includes?(last.kind)
        push(EToken::Newline, "\\n")
      end

      private def skip_comment : Nil
        until eof? || current == '\n'
          advance
        end
      end

      #--------------------------------------------------------------------------

      private def scan_number : Nil
        start_col = @col
        start_pos = @pos

        if current == '0' && (peek == 'x' || peek == 'X')
          scan_radix(start_col, start_pos, 16, "0x")
          return
        end

        if current == '0' && (peek == 'b' || peek == 'B')
          scan_radix(start_col, start_pos, 2, "0b")
          return
        end

        is_float = false
        skip_while { |c| c.ascii_number? || c == '_' }

        if current == '.' && peek.ascii_number?
          is_float = true
          advance
          skip_while { |c| c.ascii_number? || c == '_' }
        end

        if current == 'e' || current == 'E'
          is_float = true
          advance
          advance if current == '+' || current == '-'
          skip_while { |c| c.ascii_number? }
        end

        digits = @src[start_pos...@pos].gsub('_', "")
        suffix = scan_suffix
        is_float ||= suffix.starts_with?('f')

        lexeme = @src[start_pos...@pos]
        if is_float
          push(EToken::Float, lexeme, start_col, float_value: digits.to_f64, suffix: suffix)
        else
          push(EToken::Integer, lexeme, start_col, int_value: digits.to_i64, suffix: suffix)
        end
      end

      private def scan_radix ( start_col : Int32, start_pos : Int32, radix : Int32, prefix : String ) : Nil
        advance ; advance # consume the 0x / 0b prefix
        digits_start = @pos
        skip_while { |c| c.ascii_alphanumeric? || c == '_' }
        digits = @src[digits_start...@pos].gsub('_', "")
        suffix = scan_suffix
        value = digits.to_i64(radix)
        push(EToken::Integer, "#{prefix}#{digits}", start_col, int_value: value, suffix: suffix)
      end

      # A trailing type suffix such as u8 / i32 / f64. Case-normalised to lower.
      private def scan_suffix : String
        return "" unless !eof? && (current == 'u' || current == 'U' ||
                                   current == 'i' || current == 'I' ||
                                   current == 'f' || current == 'F')
        start = @pos
        advance
        skip_while { |c| c.ascii_number? }
        @src[start...@pos].downcase
      end

      #--------------------------------------------------------------------------

      private def scan_identifier : Nil
        start_col = @col
        start_pos = @pos
        skip_while { |c| c == '_' || c.ascii_alphanumeric? }
        name = @src[start_pos...@pos]
        if kw = KEYWORDS[name]?
          push(kw, name, start_col)
        else
          push(EToken::Identifier, name, start_col)
        end
      end

      #--------------------------------------------------------------------------

      private def scan_string : Nil
        start_col = @col
        advance # opening quote
        text = String.build do |io|
          until eof? || current == '"'
            if current == '\\'
              advance
              io << unescape(current)
              advance
            else
              io << current
              advance
            end
          end
        end
        if eof?
          @reporter.error("unterminated string literal", @line, start_col)
        else
          advance # closing quote
        end
        push(EToken::Str, text, start_col, text: text)
      end

      private def scan_char : Nil
        start_col = @col
        advance # opening quote
        value = '\0'
        unless eof?
          if current == '\\'
            advance
            value = unescape(current)
          else
            value = current
          end
          advance
        end
        advance if current == '\'' # closing quote
        push(EToken::Char, value.to_s, start_col, int_value: value.ord.to_i64, text: value.to_s)
      end

      private def unescape ( c : Char ) : Char
        case c
        when 'n'  then '\n'
        when 't'  then '\t'
        when 'r'  then '\r'
        when '0'  then '\0'
        when '\\' then '\\'
        when '"'  then '"'
        when '\'' then '\''
        else           c
        end
      end

      #--------------------------------------------------------------------------

      private def scan_operator : Nil
        start_col = @col
        c = current
        n = peek

        two = "#{c}#{n}"
        case two
        when "=="
          advance2(EToken::EqEq, two, start_col)
        when "!="
          advance2(EToken::NotEq, two, start_col)
        when "<="
          advance2(EToken::Le, two, start_col)
        when ">="
          advance2(EToken::Ge, two, start_col)
        when "&&"
          advance2(EToken::AndAnd, two, start_col)
        when "||"
          advance2(EToken::OrOr, two, start_col)
        when "<<"
          advance2(EToken::Shl, two, start_col)
        when ">>"
          advance2(EToken::Shr, two, start_col)
        when "+="
          advance2(EToken::PlusAssign, two, start_col)
        when "-="
          advance2(EToken::MinusAssign, two, start_col)
        when "@["
          advance2(EToken::AnnotationStart, two, start_col)
        else
          scan_single(c, start_col)
        end
      end

      private def scan_single ( c : Char, start_col : Int32 ) : Nil
        kind =
          case c
          when '+' then EToken::Plus
          when '-' then EToken::Minus
          when '*' then EToken::Star
          when '/' then EToken::Slash
          when '%' then EToken::Percent
          when '<' then EToken::Lt
          when '>' then EToken::Gt
          when '!' then EToken::Not
          when '&' then EToken::Amp
          when '|' then EToken::Pipe
          when '^' then EToken::Caret
          when '~' then EToken::Tilde
          when '=' then EToken::Assign
          when '?' then EToken::Question
          when ':' then EToken::Colon
          when '(' then EToken::LParen
          when ')' then EToken::RParen
          when '[' then EToken::LBracket
          when ']' then EToken::RBracket
          when ',' then EToken::Comma
          else
            @reporter.error("unexpected character '#{c}'", @line, start_col)
            advance
            return
          end
        advance
        push(kind, c.to_s, start_col)
      end

      private def advance2 ( kind : EToken, lexeme : String, start_col : Int32 ) : Nil
        advance ; advance
        push(kind, lexeme, start_col)
      end

      #--------------------------------------------------------------------------
      # Low-level cursor helpers
      #--------------------------------------------------------------------------

      private def eof? : Bool
        @pos >= @src.size
      end

      private def current : Char
        eof? ? '\0' : @src[@pos]
      end

      private def peek : Char
        @pos + 1 >= @src.size ? '\0' : @src[@pos + 1]
      end

      private def advance : Nil
        @pos += 1
        @col += 1
      end

      private def skip_while ( & : Char -> Bool ) : Nil
        while !eof? && (yield current)
          advance
        end
      end

      private def advance_line : Nil
        @pos += 1
        @line += 1
        @col = 1
      end

      private def push ( kind : EToken, lexeme : String, col : Int32 = @col,
                         int_value : Int64 = 0_i64, float_value : Float64 = 0.0,
                         suffix : String = "", text : String = "" ) : Nil
        @tokens << Token.new(kind, lexeme, @line, col, int_value, float_value, suffix, text)
      end

    end


  end
end
