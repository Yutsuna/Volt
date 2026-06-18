module Volt
  module Lexer


    # Numeric / textual payloads are decoded eagerly
    # by the lexer so later stages never re-parse the raw lexeme.
    struct Token

      getter kind        : EToken
      getter lexeme      : String
      getter line        : Int32
      getter col         : Int32
      getter int_value   : Int64
      getter float_value : Float64
      getter suffix      : String   # normalised numeric suffix, e.g. "u8", "f64"
      getter text        : String   # decoded char / string contents

      #--------------------------------------------------------------------------

      def initialize ( @kind : EToken, @lexeme : String, @line : Int32, @col : Int32,
                       @int_value : Int64 = 0_i64, @float_value : Float64 = 0.0,
                       @suffix : String = "", @text : String = "" )
      end

      #--------------------------------------------------------------------------

      def to_s ( io : IO ) : Nil
        io << @kind
        io << '(' << @lexeme << ')' unless @lexeme.empty?
      end

    end


  end
end
