require "./ACommand"


module Volt::CLI


  class FormatCommand < ACommand
    register "format", "Run the formatter on the code"

    property input : String?
    property check : Bool = false
    property style : String = "default"
    property config : String?
    property diff : Bool = false
    property write : Bool = false

    def execute(args : Array(String))
    end
  end


end
