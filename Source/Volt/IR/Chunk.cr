module Volt::IR


  class Chunk
    property name          : String
    property arity         : Int32
    property num_registers : Int32
    property code          : Array( Instruction )
    property constants      : Array( Value )
    property drop_map       : DropMap

    def initialize( @name : String, @arity : Int32 = 0 )
      @num_registers = 0
      @code          = [] of Instruction
      @constants     = [] of Value
      @drop_map      = DropMap.new
    end

    def disassemble( io : IO ) : Nil
      io << "chunk " << @name << " (arity=" << @arity
      io << ", regs=" << @num_registers << ")\n"
      @code.each_with_index do |ins, i|
        io << "  " << i << ": " << ins << "\n"
      end
    end
  end


end
