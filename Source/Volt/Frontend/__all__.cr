require "../Version"

require "./Types/__all__"
require "./Lexer/__all__"
require "./Diagnostic/__all__"
require "./AST/__all__"
require "./Parser/__all__"
require "./Semantic/__all__"


module Volt::Frontend


  def self.parse( source : String, file : String = "<unknown>" ) : Program
    Parser.new( source, file ).parse
  end


  def self.analyse( source : String, file : String = "<unknown>" ) : TypedProgram
    Analyser.new( parse( source, file ) ).analyse
  end


  def self.analyse( program : Program ) : TypedProgram
    Analyser.new( program ).analyse
  end


end
