require "./ACommand"

module Volt::CLI


  class VersionCommand < ACommand
    register "version", "Prints the current version of Volt"

    property input : String?

    def execute(args : Array(String))
      parse args
      Logger.info "Volt version #{Volt::VERSION}"
    end

    private def parse( args : Array(String) )
    end

  end


end
