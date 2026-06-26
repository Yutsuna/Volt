module Volt::VM


  # Arithmetic opcode handlers (typed: no tag checks — architecture #5.1).
  class Vm
    def eval_arith( op : IR::Opcode, b : IR::Value, c : IR::Value ) : IR::Value
      case op
      when .add_int?  then IR::Value.int( b.as_i + c.as_i )
      when .sub_int?  then IR::Value.int( b.as_i - c.as_i )
      when .mul_int?  then IR::Value.int( b.as_i * c.as_i )
      when .div_int?  then IR::Value.int( b.as_i // c.as_i )
      when .mod_int?  then IR::Value.int( b.as_i % c.as_i )
      when .neg_int?  then IR::Value.int( -b.as_i )
      when .add_f64?  then IR::Value.float( b.as_f + c.as_f )
      when .sub_f64?  then IR::Value.float( b.as_f - c.as_f )
      when .mul_f64?  then IR::Value.float( b.as_f * c.as_f )
      when .div_f64?  then IR::Value.float( b.as_f / c.as_f )
      when .neg_f64?  then IR::Value.float( -b.as_f )
      when .and_int?  then IR::Value.int( b.as_i & c.as_i )
      when .or_int?   then IR::Value.int( b.as_i | c.as_i )
      when .xor_int?  then IR::Value.int( b.as_i ^ c.as_i )
      when .shl_int?  then IR::Value.int( b.as_i << c.as_i )
      when .shr_int?  then IR::Value.int( b.as_i >> c.as_i )
      when .not_int?  then IR::Value.int( ~b.as_i )
      when .idiv_int? then IR::Value.int( b.as_i // c.as_i )
      when .pow_int?  then IR::Value.int( b.as_i ** c.as_i )
      else
        raise VoltRuntimeError.new( "unhandled arithmetic opcode #{op}" )
      end
    end
  end


end
