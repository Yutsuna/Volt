module Volt
  module Codegen


    class FCodegen

      #--------------------------------------------------------------------------
      # Statements
      #--------------------------------------------------------------------------

      private def gen_node ( node : Ast::Node ) : Nil
        case node
        when Ast::ExprStmt then gen_expr(node.expr)
        when Ast::Return   then gen_return(node)
        when Ast::If       then gen_if(node)
        when Ast::While    then gen_while(node)
        when Ast::Expr     then gen_expr(node)
        end
      end

      private def gen_return ( node : Ast::Return ) : Nil
        if value = node.value
          v = gen_expr(value)
          emit "ret #{type_of(value).llvm} #{v}"
        else
          emit "ret void"
        end
        @terminated = true
      end

      private def gen_if ( node : Ast::If ) : Nil
        cond = gen_expr(node.condition)
        then_l = new_label
        end_l  = new_label
        else_body = node.else_body
        else_l = else_body ? new_label : end_l

        emit_term "br i1 #{cond}, label %#{then_l}, label %#{else_l}"

        start_block(then_l)
        gen_body(node.then_body)
        branch(end_l) unless @terminated

        if else_body
          start_block(else_l)
          gen_body(else_body)
          branch(end_l) unless @terminated
        end

        start_block(end_l)
      end

      private def gen_while ( node : Ast::While ) : Nil
        cond_l = new_label
        body_l = new_label
        end_l  = new_label

        branch(cond_l)
        start_block(cond_l)
        c = gen_expr(node.condition)
        if node.is_until
          emit_term "br i1 #{c}, label %#{end_l}, label %#{body_l}"
        else
          emit_term "br i1 #{c}, label %#{body_l}, label %#{end_l}"
        end

        start_block(body_l)
        gen_body(node.body)
        branch(cond_l) unless @terminated

        start_block(end_l)
      end

      private def gen_body ( body : Array(Ast::Node) ) : Nil
        body.each { |n| gen_node(n) }
      end

    end

  end
end
