module Volt::VM


  # Comparison opcode handlers. EQ/NE are value equality over any type; the ordered
  # comparisons are typed (int / f64) per the opcode the compiler chose.
  class Vm
    def eval_cmp( op : IR::Opcode, b : IR::Value, c : IR::Value ) : IR::Value
      case op
      when .eq?            then IR::Value.bool( b == c )
      when .ne?            then IR::Value.bool( !( b == c ) )
      when .lt_int?        then IR::Value.bool( b.as_i < c.as_i )
      when .le_int?        then IR::Value.bool( b.as_i <= c.as_i )
      when .gt_int?        then IR::Value.bool( b.as_i > c.as_i )
      when .ge_int?        then IR::Value.bool( b.as_i >= c.as_i )
      when .lt_f64?        then IR::Value.bool( b.as_f < c.as_f )
      when .le_f64?        then IR::Value.bool( b.as_f <= c.as_f )
      when .gt_f64?        then IR::Value.bool( b.as_f > c.as_f )
      when .ge_f64?        then IR::Value.bool( b.as_f >= c.as_f )
      when .cmp_int?       then IR::Value.int( ( b.as_i <=> c.as_i ).to_i64 )
      when .eq_case?
        res = if b.raw.is_a?( ::Regex ) && c.raw.is_a?( String )
          b.as_regex.matches?( c.as_s )
        else
          b == c
        end
        IR::Value.bool( res )
      when .match_str?     then IR::Value.bool( c.as_regex.matches?( b.as_s ) )
      when .not_match_str? then IR::Value.bool( !c.as_regex.matches?( b.as_s ) )
      else
        raise VoltRuntimeError.new( "unhandled comparison opcode #{op}" )
      end
    end
  end


end
