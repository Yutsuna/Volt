module Volt::IR


  # Fixed-width 32-bit packed instruction (architecture #6.1):
  #
  #   31     24 23    16 15     8 7      0
  #  ┌─────────┬────────┬────────┬────────┐
  #  │ opcode  │   A    │   B    │   C    │   3-register form
  #  ├─────────┼────────┼────────┴────────┤
  #  │ opcode  │   A    │      Bx (16)    │   reg + 16-bit operand
  #  └─────────┴────────┴─────────────────┘
  struct Instruction
    getter raw : UInt32

    def initialize( @raw : UInt32 )
    end

    def self.abc( op : Opcode, a : Int32, b : Int32, c : Int32 ) : Instruction
      new( ( op.value.to_u32 << 24 ) | ( ( a & 0xFF ).to_u32 << 16 ) |
           ( ( b & 0xFF ).to_u32 << 8 ) | ( c & 0xFF ).to_u32 )
    end

    def self.abx( op : Opcode, a : Int32, bx : Int32 ) : Instruction
      new( ( op.value.to_u32 << 24 ) | ( ( a & 0xFF ).to_u32 << 16 ) |
           ( bx & 0xFFFF ).to_u32 )
    end

    def op : Opcode
      Opcode.new( ( raw >> 24 ).to_u8 )
    end

    def a : Int32  ; ( ( raw >> 16 ) & 0xFF ).to_i32 ; end
    def b : Int32  ; ( ( raw >> 8 ) & 0xFF ).to_i32  ; end
    def c : Int32  ; ( raw & 0xFF ).to_i32           ; end
    def bx : Int32 ; ( raw & 0xFFFF ).to_i32         ; end

    def to_s( io : IO ) : Nil
      io << op
      io << " A=" << a << " B=" << b << " C=" << c
    end
  end


end
