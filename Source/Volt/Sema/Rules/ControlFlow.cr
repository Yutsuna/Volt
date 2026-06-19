module Volt
  module Sema


    module Rules::ControlFlow

      #--------------------------------------------------------------------------

      private def analyze_def( d : Ast::Def ) : Nil
        @context.with_function( d ) do
          @context.with_scope do

            d.params.each { |p| @context.current_scope.define_param( p.name, p.ptype ) }
            d.body.each { |node| analyze_node( node ) }

            try_inject_implicit_return( d.body, d.return_type )
            ensure_return_statement( d.body, d.return_type )

          end
        end
      end

      private def try_inject_implicit_return( body : Array(Ast::Node), return_type : Types::Type ) : Nil
        last = body.last?
        if !return_type.void? && last.is_a?( Ast::ExprStmt )
          body[ body.size - 1 ] = Ast::Return.new last.expr
        end
      end

      private def ensure_return_statement( body : Array(Ast::Node), return_type : Types::Type ) : Nil
        last = body.last?
        if !return_type.void? && ( last.nil? || !last.is_a?( Ast::Return ) )
          line = last ? last.line : 0
          col = last ? last.col : 0
          @reporter.error( "missing return statement in function of type '#{return_type}'", line, col )
        end
      end

      #--------------------------------------------------------------------------

      private def analyze_body( body : Array(Ast::Node) ) : Nil
        body.each { |node| analyze_node( node ) }
      end

      #--------------------------------------------------------------------------

      private def analyze_node( node : Ast::Node ) : Nil
        case node
        when Ast::ExprStmt
          node.expr = analyze_expr( node.expr )
        when Ast::Return
          v = node.value
          node.value = analyze_expr( v ) if v
          check_return_statement( node, @context.current_function, node.line, node.col )
          @context.terminated = true
        when Ast::If
          node.condition = analyze_expr( node.condition )
          @context.with_scope { analyze_body( node.then_body ) }
          if eb = node.else_body
            @context.with_scope { analyze_body( eb ) }
          end
        when Ast::While
          node.condition = analyze_expr( node.condition )
          @context.with_loop { @context.with_scope { analyze_body( node.body ) } }
        when Ast::Expr
          analyze_expr(node)
        end
      end

      #--------------------------------------------------------------------------

      private def check_return_statement(node : Ast::Return, cur_func : Ast::Def?, line : Int32, col : Int32) : Nil
        if cur_func.nil?
          @reporter.error( "return statement outside of any function", line, col )
          return
        end

        ret_type = node.value ? type_of(node.value.not_nil!) : Types::Type.new(Types::EType::Void)
        if ret_type != cur_func.return_type
          @reporter.error( "type mismatch: expected return type '#{cur_func.return_type}' but got '#{ret_type}'", line, col )
        end
      end

    end


  end
end
