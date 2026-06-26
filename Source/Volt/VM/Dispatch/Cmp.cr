module Volt::VM


  # Comparison opcode handlers. EQ/NE are value equality over any type; the ordered
  # comparisons are typed (int / f64) per the opcode the compiler chose.
  class Vm
    def eval_cmp( op : IR::Opcode, b : IR::Value, c : IR::Value ) : IR::Value
      result =
        case op
        when .eq?     then b == c
        when .ne?     then !( b == c )
        when .lt_int? then b.as_i < c.as_i
        when .le_int? then b.as_i <= c.as_i
        when .gt_int? then b.as_i > c.as_i
        when .ge_int? then b.as_i >= c.as_i
        when .lt_f64? then b.as_f < c.as_f
        when .le_f64? then b.as_f <= c.as_f
        when .gt_f64? then b.as_f > c.as_f
        when .ge_f64? then b.as_f >= c.as_f
        else
          raise VoltRuntimeError.new( "unhandled comparison opcode #{op}" )
        end
      IR::Value.bool( result )
    end
  end


end
