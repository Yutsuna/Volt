module Volt::Compiler

  record NativeFunc, lib : String?, name : String

  class Unit
    property chunks     : Array( IR::Chunk )
    property main_index : Int32
    property natives    : Array( NativeFunc )
    property classes    : Array( Runtime::ObjectModel::RClass )

    def initialize( @chunks : Array( IR::Chunk ), @main_index : Int32,
                    @natives : Array( NativeFunc ) = [] of NativeFunc,
                    @classes : Array( Runtime::ObjectModel::RClass ) = [] of Runtime::ObjectModel::RClass )
    end

    def disassemble( io : IO ) : Nil
      @chunks.each_with_index do |chunk, i|
        io << "; [" << i << "]" << ( i == @main_index ? " (entry)" : "" ) << "\n"
        chunk.disassemble( io )
        io << "\n"
      end
    end
  end

end
