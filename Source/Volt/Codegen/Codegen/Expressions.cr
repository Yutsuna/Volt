module Volt
  module Codegen


    # Conventions:
    #   - extern C functions keep their verbatim name (`@puts`).
    #   - Volt defs are mangled (`@"volt.name"`) to avoid clashing with C `main`.
    #   - top-level statements form the synthesized C `define i32 @main()`.
    #   - variable slots live in `%s.<slot>` allocas; SSA temps are `%tN`.
    class FCodegen

      #--------------------------------------------------------------------------
      # Expressions
      #--------------------------------------------------------------------------

      private def gen_expr( expr : Ast::Expr ) : String
        case expr
        when Ast::IntLit    then expr.value.to_s
        when Ast::FloatLit  then f64_hex( expr.value, type_of( expr ) )
        when Ast::BoolLit   then expr.value ? "true" : "false"
        when Ast::CharLit   then expr.value.to_s
        when Ast::StrLit    then gen_str( expr )
        when Ast::NilLit    then "0"
        when Ast::ArrayLit  then gen_array( expr )
        when Ast::VarRef    then gen_varref( expr )
        when Ast::Assign    then gen_assign( expr )
        when Ast::BinaryOp  then gen_binary( expr )
        when Ast::UnaryOp   then gen_unary( expr )
        when Ast::Ternary   then gen_ternary( expr )
        when Ast::Call      then gen_call( expr )
        when Ast::PointerOf then gen_pointerof( expr )
        when Ast::Cast      then gen_cast( expr )
        when Ast::Index     then gen_index( expr )
        else                     "0"
        end
      end

      private def gen_str( expr : Ast::StrLit ) : String
        name, len = @emitter.intern( expr.value)
        "getelementptr inbounds ([#{len} x i8], [#{len} x i8]* #{name}, i32 0, i32 0)"
      end

      private def gen_array( expr : Ast::ArrayLit ) : String
        n        = expr.elements.size
        base_ty  = Types::Type.new( type_of( expr ).base )
        elem_ty  = base_ty.llvm
        arr_ty   = "[#{n + 1} x #{elem_ty}]"
        slot     = "arr.#{next_seq}"
        ensure_alloca( slot, arr_ty )

        expr.elements.each_with_index do |el, i|
          v = gen_expr( el )
          p = new_temp
          emit "#{p} = getelementptr inbounds #{arr_ty}, #{arr_ty}* %s.#{slot}, i64 0, i64 #{i}"
          emit "store #{elem_ty} #{v}, #{elem_ty}* #{p}"
        end

        # Stocker le terminateur nul (nil) à l'index n
        p = new_temp
        zero = zero_value( base_ty )
        emit "#{p} = getelementptr inbounds #{arr_ty}, #{arr_ty}* %s.#{slot}, i64 0, i64 #{n}"
        emit "store #{elem_ty} #{zero}, #{elem_ty}* #{p}"

        ptr = new_temp
        emit "#{ptr} = getelementptr inbounds #{arr_ty}, #{arr_ty}* %s.#{slot}, i64 0, i64 0"
        ptr
      end

      private def gen_varref( expr : Ast::VarRef ) : String
        ty = type_of( expr ).llvm
        t  = new_temp
        emit "#{t} = load #{ty}, #{ty}* %s.#{expr.slot}"
        t
      end

      private def gen_assign( expr : Ast::Assign ) : String
        v  = gen_expr( expr.value )
        ty = type_of( expr ).llvm
        ensure_alloca( expr.slot, ty)
        emit "store #{ty} #{v}, #{ty}* %s.#{expr.slot}"
        v
      end

      private def gen_binary( expr : Ast::BinaryOp ) : String
        l       = gen_expr( expr.left )
        r       = gen_expr( expr.right )
        operand = type_of( expr.left )
        instr   = binary_instr( expr.op, operand )
        t = new_temp
        emit "#{t} = #{instr} #{operand.llvm} #{l}, #{r}"
        t
      end

      private def gen_unary( expr : Ast::UnaryOp ) : String
        v  = gen_expr( expr.operand )
        ty = type_of( expr.operand )
        t  = new_temp
        case expr.op
        when "!"
          emit "#{t} = xor i1 #{v}, true"
        when "~"
          emit "#{t} = xor #{ty.llvm} #{v}, -1"
        when "-"
          if ty.float?
            emit "#{t} = fneg #{ty.llvm} #{v}"
          else
            emit "#{t} = sub #{ty.llvm} 0, #{v}"
          end
        end
        t
      end

      private def gen_ternary( expr : Ast::Ternary ) : String
        ty   = type_of( expr ).llvm
        slot = "tern.#{next_seq}"
        ensure_alloca( slot, ty )

        cond   = gen_expr( expr.condition )
        then_l = new_label
        else_l = new_label
        end_l  = new_label

        emit_term "br i1 #{cond}, label %#{then_l}, label %#{else_l}"

        start_block( then_l)
        tv = gen_expr( expr.then_expr )
        emit "store #{ty} #{tv}, #{ty}* %s.#{slot}" unless @terminated
        branch( end_l ) unless @terminated

        start_block( else_l )
        ev = gen_expr( expr.else_expr )
        emit "store #{ty} #{ev}, #{ty}* %s.#{slot}" unless @terminated
        branch( end_l ) unless @terminated

        start_block( end_l )
        res = new_temp
        emit "#{res} = load #{ty}, #{ty}* %s.#{slot}"
        res
      end

      private def gen_call( expr : Ast::Call ) : String
        symbol, ret = resolve_symbol( expr.name )
        args = expr.args.map { |a| "#{type_of(a).llvm} #{gen_expr(a)}" }.join(", ")
        if ret.void?
          emit "call void #{symbol}(#{args})"
          ""
        else
          t = new_temp
          emit "#{t} = call #{ret.llvm} #{symbol}(#{args})"
          t
        end
      end

      private def gen_pointerof( expr : Ast::PointerOf ) : String
        operand = expr.operand
        if operand.is_a?( Ast::VarRef )
          "%s.#{operand.slot}"
        else
          "null"
        end
      end

      private def gen_cast( expr : Ast::Cast ) : String
        v         = gen_expr( expr.operand )
        from_type = type_of( expr.operand )
        to_type   = type_of( expr )
        from      = from_type.llvm
        to        = to_type.llvm
        t         = new_temp

        if from_type.integer? && to_type.float?
          emit "#{t} = sitofp #{from} #{v} to #{to}"
        elsif from_type.float? && to_type.float?
          if from_type.base == Types::EType::Float32 && to_type.base == Types::EType::Float64
            emit "#{t} = fpext #{from} #{v} to #{to}"
          elsif from_type.base == Types::EType::Float64 && to_type.base == Types::EType::Float32
            emit "#{t} = fptrunc #{from} #{v} to #{to}"
          else
            return v
          end
        else
          emit "#{t} = sitofp #{from} #{v} to #{to}"
        end
        t
      end

      private def gen_index( expr : Ast::Index ) : String
        ptr      = gen_expr(expr.pointer)
        idx      = gen_expr(expr.index)
        ptr_type = type_of(expr.pointer)
        elem_ty  = type_of(expr).llvm
        idx_ty   = type_of(expr.index).llvm

        ptr_temp = new_temp
        emit "#{ptr_temp} = getelementptr inbounds #{elem_ty}, #{ptr_type.llvm} #{ptr}, #{idx_ty} #{idx}"

        val_temp = new_temp
        emit "#{val_temp} = load #{elem_ty}, #{elem_ty}* #{ptr_temp}"
        val_temp
      end

      #--------------------------------------------------------------------------

    end


  end
end
