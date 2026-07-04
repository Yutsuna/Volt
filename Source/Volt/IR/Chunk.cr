module Volt::IR


  class Chunk

    property name           : String
    property arity          : Int32
    property num_registers  : Int32
    property code           : Array( Instruction )
    property constants      : Array( Value )
    property drop_map       : DropMap
    property scope_tables   : Array( Array( Int32 ) )
    # Out-of-line operands for `CALL_MIXIN` (see `CallSite`) : indexed by the
    # instruction's `Bx`.
    property call_sites     : Array( CallSite )

    def initialize( @name : String, @arity : Int32 = 0 )
      @num_registers = 0
      @code          = [] of Instruction
      @constants     = [] of Value
      @drop_map      = DropMap.new
      @scope_tables  = [] of Array( Int32 )
      @call_sites    = [] of CallSite
    end

    @raii_regs : Array( Int32 )? = nil

    def raii_regs : Array( Int32 )
      cached = @raii_regs
      return cached if cached
      computed = [] of Int32
      @drop_map.entries.each { |e| computed << e.register.to_i32 }
      @scope_tables.each { |t| computed.concat( t ) }
      @raii_regs = computed
      computed
    end

    def disassemble( io : IO ) : Nil
      io << "chunk " << @name << " (arity=" << @arity
      io << ", regs=" << @num_registers << ")\n"
      @code.each_with_index { |ins, i| io << "  " << i << ": " << ins << "\n" }
    end

  end


end
