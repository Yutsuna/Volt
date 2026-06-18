module Volt
  module Diagnostic


    class CompileError < Exception
    end


    class FReporter

      getter? had_error : Bool

      #--------------------------------------------------------------------------

      def initialize ( @file : String )
        @had_error = false
      end

      #--------------------------------------------------------------------------

      def error ( message : String, line : Int32 = 0, col : Int32 = 0 ) : Nil
        @had_error = true
        FLog.error("#{location(line, col)}: #{message}")
      end

      def abort! ( message : String, line : Int32 = 0, col : Int32 = 0 )
        error(message, line, col)
        raise CompileError.new(message)
      end

      #--------------------------------------------------------------------------

      private def location ( line : Int32, col : Int32 ) : String
        return @file if line <= 0
        "#{@file}:#{line}:#{col}"
      end

    end


  end
end
