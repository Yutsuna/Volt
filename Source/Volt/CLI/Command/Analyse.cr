require "./ACommand"


module Volt::CLI


  class AnalyseCommand < ACommand
    register "analyse", "Static semantic code analysis"

    property input : String?
    property type : String = "syntax"
    property rules : String?
    property output : String = "json"
    property warn_as_error : Bool = false
    property metrics : Bool = false
    property unused : Bool = false

    def execute(args : Array(String))
    end
  end


end
