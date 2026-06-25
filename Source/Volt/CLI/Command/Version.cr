require "./ACommand"

module Volt


  VERSION = "0.1.0"


  module CLI


    class Version < ACommand

      register "version", "Display the usage"

      def execute(args : Array(String))
        puts "Volt current version: #{VERSION}"
      end
    end


  end

end
