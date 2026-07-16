require "../Version"

require "./Types/__all__"
require "./Symbols"
require "./Lexer/__all__"
require "./Diagnostic/__all__"
require "./AST/__all__"
require "./Parser/__all__"
require "./Semantic/__all__"


module Volt::Frontend


  @@core_files = Set( String ).new


  def self.core_files : Set( String )
    @@core_files
  end


  def self.core_file?( path : String ) : Bool
    @@core_files.includes?( path )
  end


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
