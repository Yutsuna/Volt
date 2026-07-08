module Volt::VM


  # Arithmetic opcode handlers (typed: no tag checks : architecture #5.1).
  class Vm
    # Dispatch on enum *constants* (not `.pred?` predicates) so Crystal/LLVM
    # lowers this to a jump table rather than a linear if-chain.
    def eval_arith( op : IR::Opcode, b : IR::Value, c : IR::Value ) : IR::Value
      case op
      when IR::Opcode::ADD_INT  then IR::Value.int( b.as_i + c.as_i )
      when IR::Opcode::SUB_INT  then IR::Value.int( b.as_i - c.as_i )
      when IR::Opcode::MUL_INT  then IR::Value.int( b.as_i * c.as_i )
      when IR::Opcode::DIV_INT  then IR::Value.int( b.as_i // c.as_i )
      when IR::Opcode::MOD_INT  then IR::Value.int( b.as_i % c.as_i )
      when IR::Opcode::NEG_INT  then IR::Value.int( -b.as_i )
      when IR::Opcode::ADD_F64  then IR::Value.float( b.as_f + c.as_f )
      when IR::Opcode::SUB_F64  then IR::Value.float( b.as_f - c.as_f )
      when IR::Opcode::MUL_F64  then IR::Value.float( b.as_f * c.as_f )
      when IR::Opcode::DIV_F64  then IR::Value.float( b.as_f / c.as_f )
      when IR::Opcode::NEG_F64  then IR::Value.float( -b.as_f )
      when IR::Opcode::AND_INT  then IR::Value.int( b.as_i & c.as_i )
      when IR::Opcode::OR_INT   then IR::Value.int( b.as_i | c.as_i )
      when IR::Opcode::XOR_INT  then IR::Value.int( b.as_i ^ c.as_i )
      when IR::Opcode::SHL_INT  then IR::Value.int( b.as_i << c.as_i )
      when IR::Opcode::SHR_INT  then IR::Value.int( b.as_i >> c.as_i )
      when IR::Opcode::NOT_INT  then IR::Value.int( ~b.as_i )
      when IR::Opcode::IDIV_INT then IR::Value.int( b.as_i // c.as_i )
      when IR::Opcode::POW_INT  then IR::Value.int( b.as_i ** c.as_i )
      else
        raise VoltRuntimeError.new( "unhandled arithmetic opcode #{op}" )
      end
    end
  end


end
