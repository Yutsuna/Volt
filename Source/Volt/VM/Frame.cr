module Volt::VM


  # A call frame is a *window* into the VM's shared contiguous register stack
  # (architecture: the per-frame `Array` of v0.1.0 is gone). `Frame` is a thin,
  # allocation-free value: it only carries the base pointer of this frame's
  # register block inside `Vm#@stack`. Register access is a raw pointer index
  # (no bounds check) and never owns memory — the stack outlives every frame.
  struct Frame
    def initialize( @regs : Pointer( IR::Value ) )
    end

    @[AlwaysInline]
    def []( i : Int32 ) : IR::Value
      @regs[ i ]
    end

    @[AlwaysInline]
    def []=( i : Int32, v : IR::Value )
      @regs[ i ] = v
    end

    # Zero-copy view into this frame's register window (used by `CALL_NATIVE`'s
    # `collect_args` — no per-call `Array` allocation, architecture #9 Phase 5).
    @[AlwaysInline]
    def slice( offset : Int32, count : Int32 ) : Slice( IR::Value )
      Slice.new( @regs + offset, count )
    end
  end


end
