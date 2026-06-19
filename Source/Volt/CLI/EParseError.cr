module Volt
  module CLI


    class EParseError < Exception

      def initialize( message : String, @code : Int32 = 1 )
        super message
      end

    end


  end
end
