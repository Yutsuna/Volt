require "../Compiler/Unit"

module Volt::REPL


  class IncrementalState
    getter top_level_globals : Hash(String, Frontend::Type)
    getter signatures : Hash(String, Frontend::FuncSig)
    getter types : Hash(String, Frontend::TypeInfo)
    getter nominals : Hash(String, Frontend::NominalType)

    getter func_index : Hash(String, Int32)
    getter global_index : Hash(String, Int32)
    getter natives : Array(Compiler::NativeFunc)
    property chunk_count : Int32
    property globals_count : Int32

    def initialize
      @top_level_globals = {} of String => Frontend::Type
      @signatures = {} of String => Frontend::FuncSig
      @types = {} of String => Frontend::TypeInfo
      @nominals = {} of String => Frontend::NominalType

      @func_index = {} of String => Int32
      @global_index = {} of String => Int32
      @natives = [] of Compiler::NativeFunc
      @chunk_count = 0
      @globals_count = 0
    end

    def clear : Nil
      @top_level_globals.clear
      @signatures.clear
      @types.clear
      @nominals.clear
      @func_index.clear
      @global_index.clear
      @natives.clear
      @chunk_count = 0
      @globals_count = 0
    end

    def add_global( name : String, type : Frontend::Type ) : Nil
      @top_level_globals[ name ] = type
    end

    def has_global?( name : String ) : Bool
      @top_level_globals.has_key?( name )
    end

    def get_global( name : String ) : Frontend::Type?
      @top_level_globals[ name ]?
    end

    def add_signature( name : String, sig : Frontend::FuncSig ) : Nil
      @signatures[ name ] = sig
    end

    def add_type( name : String, type_info : Frontend::TypeInfo ) : Nil
      @types[ name ] = type_info
    end

    def add_nominal( name : String, nominal_type : Frontend::NominalType ) : Nil
      @nominals[ name ] = nominal_type
    end
  end


end

