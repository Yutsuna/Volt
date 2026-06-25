require "./Lexer/__all__"
require "./AST/__all__"
require "./Parser/__all__"


module Volt::Frontend


  def self.parse( source : String, file : String = "<unknown>" ) : Program
    Parser.new( source, file ).parse
  end


end
