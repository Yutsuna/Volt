require "./FCommands"
require "./EParseError"

module Volt
  module CLI


    module FCLI

      extend self

      def run ( args : Array(String) ) : Int32
        begin
          FCommands.execute!( args )
        rescue exception : EParseError
          FLog.error exception.message.to_s
          1
        end
      end

    end


  end
end
