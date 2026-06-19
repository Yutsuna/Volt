module Volt
  module Sema


    module Rules::Declarations

      #--------------------------------------------------------------------------

      private def analyze_varref( expr : Ast::VarRef ) : Ast::Expr
        if found = @context.current_scope.lookup( expr.name )
          expr.slot = found[ 0 ]
          expr.type = found[ 1 ]
          return expr
        end

        if zero_arg_callable?( expr.name )
          return analyze_expr( Ast::Call.new( expr.name, [] of Ast::Expr ) )
        end

        @reporter.error( "undefined variable '#{expr.name}'", expr.line, expr.col )
        expr.type = Types::Type.new( Types::EType::Int32 )
        expr
      end

      #--------------------------------------------------------------------------

      private def analyze_assign( expr : Ast::Assign ) : Ast::Expr
        expr.value = analyze_expr( expr.value )
        vtype = expr.declared_type || type_of( expr.value )

        if existing = @context.current_scope.lookup( expr.name )
          check_assignment_types( existing[ 1 ], vtype, expr.line, expr.col )
        end

        expr.slot = @context.current_scope.assign( expr.name, vtype )
        expr.type = vtype
        expr
      end

      #--------------------------------------------------------------------------

      private def analyze_pointerof( expr : Ast::PointerOf ) : Ast::Expr
        expr.operand = analyze_expr( expr.operand )
        expr.type = type_of( expr.operand ).to_pointer
        expr
      end

      #--------------------------------------------------------------------------

      private def analyze_index( expr : Ast::Index ) : Ast::Expr
        expr.pointer = analyze_expr(expr.pointer)
        expr.index   = analyze_expr(expr.index)

        ptr_type = type_of( expr.pointer )
        if ptr_type.pointer?
          expr.type = Types::Type.new( ptr_type.base, ptr_type.pointer_depth - 1 )
        else
          @reporter.error( "cannot index non-pointer type '#{ptr_type}'", expr.line, expr.col )
          expr.type = Types::Type.new( Types::EType::Int32 )
        end
        expr
      end

      #--------------------------------------------------------------------------

      private def zero_arg_callable?( name : String ) : Bool
        if d = @defs[ name ]?
          return d.params.empty?
        end
        if e = @externs[ name ]?
          return e.params.empty?
        end
        false
      end

      #--------------------------------------------------------------------------

      private def call_return_type( name : String, call : Ast::Call ) : Types::Type
        if d = @defs[ name ]?
          return d.return_type
        end
        if e = @externs[ name ]?
          return e.return_type
        end
        case name
        when "puts" then Types::Type.new( Types::EType::Int32 )
        when "exit" then Types::Type.new( Types::EType::Nil )
        else
          @reporter.error( "undefined function '#{name}'", call.line, call.col )
          Types::Type.new( Types::EType::Int32 )
        end
      end

      #--------------------------------------------------------------------------

    end


  end
end
