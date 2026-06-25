require "./ACommand"


module Volt::CLI


  class REPLCommand < ACommand
    register "repl", "Read-Eval-Print-Loop"

    property input : String?

    def execute(args : Array(String))
    end
  end


end
