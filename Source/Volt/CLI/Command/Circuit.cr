require "./ACommand"


module Volt::CLI


  class CircuitCommand < ACommand
    register "circuit", "Create or update the Project.vl file"

    property project_directory : String?

    def execute(args : Array(String))
    end
  end


end
