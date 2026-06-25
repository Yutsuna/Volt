require "./ACommand"


module Volt::CLI


  class RunCommand < ACommand
    register "run", "Interpret the file input"

    property input : String?
    property program_args = [] of String

    def execute(args : Array(String))
    end
  end


end
