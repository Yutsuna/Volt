require "./ACommand"


module Volt::CLI


  class BuildCommand < ACommand
    register "build", "Compile the file input"

    property output : String = "main.bin"
    property release : Bool = false
    property defines = [] of String
    property libs = [] of String

    def execute(args : Array(String))
    end
  end


end
