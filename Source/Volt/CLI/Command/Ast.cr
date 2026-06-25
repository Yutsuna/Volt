require "./ACommand"


module Volt::CLI


  class AstCommand < ACommand
    register "ast", "Generate & display the abstract syntax tree"

    property input : String?
    property output : String?
    property format : String = "json"
    property simplify : Bool = false
    property color : Bool = false
    property no_location : Bool = false

    def execute(args : Array(String))
    end
  end


end
