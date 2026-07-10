module Volt::VM


  # Comparison opcode handlers. EQ/NE are value equality over any type; the ordered
  # comparisons are typed (int / f64) per the opcode the compiler chose.
  class Vm
    # Dispatch on enum *constants* (not `.pred?` predicates) so Crystal/LLVM
    # lowers this to a jump table rather than a linear if-chain.
    def eval_cmp( op : IR::Opcode, b : IR::Value, c : IR::Value ) : IR::Value
      case op
      when IR::Opcode::EQ            then IR::Value.bool( b == c )
      when IR::Opcode::NE            then IR::Value.bool( !( b == c ) )
      when IR::Opcode::EQ_INT        then IR::Value.bool( b.as_i == c.as_i )
      when IR::Opcode::NE_INT        then IR::Value.bool( b.as_i != c.as_i )
      when IR::Opcode::LT_INT        then IR::Value.bool( b.as_i < c.as_i )
      when IR::Opcode::LE_INT        then IR::Value.bool( b.as_i <= c.as_i )
      when IR::Opcode::GT_INT        then IR::Value.bool( b.as_i > c.as_i )
      when IR::Opcode::GE_INT        then IR::Value.bool( b.as_i >= c.as_i )
      when IR::Opcode::LT_F64        then IR::Value.bool( b.as_f < c.as_f )
      when IR::Opcode::LE_F64        then IR::Value.bool( b.as_f <= c.as_f )
      when IR::Opcode::GT_F64        then IR::Value.bool( b.as_f > c.as_f )
      when IR::Opcode::GE_F64        then IR::Value.bool( b.as_f >= c.as_f )
      when IR::Opcode::CMP_INT       then IR::Value.int( ( b.as_i <=> c.as_i ).to_i64 )
      when IR::Opcode::EQ_CASE
        res = if b.tag == IR::Value::Tag::Regex && ( cs = volt_string( c ) )
          b.as_regex.matches?( cs )
        else
          b == c
        end
        IR::Value.bool( res )
      when IR::Opcode::MATCH_STR     then IR::Value.bool( c.as_regex.matches?( volt_string( b ).not_nil! ) )
      when IR::Opcode::NOT_MATCH_STR then IR::Value.bool( !c.as_regex.matches?( volt_string( b ).not_nil! ) )
      else
        raise VoltRuntimeError.new( "unhandled comparison opcode #{op}" )
      end
    end
  end


end
