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
          FLog.error "Use 'volt help' for usage."
          1
        end
      end

    end


  end
end
