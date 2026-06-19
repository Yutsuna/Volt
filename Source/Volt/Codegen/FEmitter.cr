module Volt
  module Codegen


    class FEmitter

      def initialize
        @pool    = {} of String => {String, Int32}
        @decls   = [] of String
        @counter = 0
      end

      #--------------------------------------------------------------------------

      def intern ( content : String ) : {String, Int32}
        if cached = @pool[content]?
          return cached
        end
        bytes = content.bytes
        len   = bytes.size + 1
        name  = "@.str.#{@counter}"
        @counter += 1
        @decls << "#{name} = private unnamed_addr constant [#{len} x i8] c\"#{escape(bytes)}\""
        result = {name, len}
        @pool[content] = result
        result
      end

      def declarations : Array(String)
        @decls
      end

      #--------------------------------------------------------------------------

      private def escape ( bytes : Array(UInt8) ) : String
        String.build do |io|
          bytes.each do |b|
            if b >= 0x20_u8 && b < 0x7F_u8 && b != 0x22_u8 && b != 0x5C_u8
              io << b.unsafe_chr
            else
              io << "\\" << ("%02X" % b)
            end
          end
          io << "\\00"
        end
      end

      #--------------------------------------------------------------------------

    end


  end
end
