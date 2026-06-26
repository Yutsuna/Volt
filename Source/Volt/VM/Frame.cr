module Volt::VM


  # A call frame: a register window over its chunk. v0.1.0 gives each frame its own register
  # array (recursive interpreter); the shared contiguous register stack
  # is a later optimization that keeps this register-access API.
  class Frame
    getter chunk : IR::Chunk
    getter regs  : Array( IR::Value )

    def initialize( @chunk : IR::Chunk, args : Array( IR::Value ) )
      size  = @chunk.num_registers
      size  = args.size if args.size > size
      @regs = Array( IR::Value ).new( size ) { IR::Value.nil_value }
      args.each_with_index { |a, i| @regs[ i ] = a }
    end

    def []( i : Int32 ) : IR::Value
      @regs[ i ]
    end

    def []=( i : Int32, v : IR::Value )
      @regs[ i ] = v
    end
  end


end
