module Volt
  module Sema


    module Rules::Expressions

      #--------------------------------------------------------------------------

      private def analyze_expr( expr : Ast::Expr ) : Ast::Expr
        case expr
        when Ast::IntLit, Ast::FloatLit, Ast::BoolLit, Ast::CharLit,
              Ast::StrLit, Ast::NilLit
          expr
        when Ast::ArrayLit  then analyze_array( expr )
        when Ast::VarRef    then analyze_varref( expr )
        when Ast::Assign    then analyze_assign( expr )
        when Ast::BinaryOp  then analyze_binary( expr )
        when Ast::UnaryOp   then analyze_unary( expr )
        when Ast::Ternary   then analyze_ternary( expr )
        when Ast::Call      then analyze_call( expr )
        when Ast::TypeOf    then analyze_typeof( expr )
        when Ast::PointerOf then analyze_pointerof( expr )
        when Ast::Index     then analyze_index( expr )
        else                     expr
        end
      end

      #--------------------------------------------------------------------------

      private def analyze_array( expr : Ast::ArrayLit ) : Ast::Expr
        expr.elements = expr.elements.map { |e| analyze_expr( e ) }
        elem = expr.elements.first?
        base = elem ? type_of( elem ).base : Types::EType::Int32
        expr.type = Types::Type.new( base, 1 )
        expr
      end

      #--------------------------------------------------------------------------

      private def analyze_call( expr : Ast::Call ) : Ast::Expr
        expr.args = expr.args.map { |a| analyze_expr( a ) }
        expr.type = call_return_type( expr.name, expr )
        expr
      end

      #--------------------------------------------------------------------------

      private def analyze_typeof( expr : Ast::TypeOf ) : Ast::Expr
        operand = analyze_expr( expr.operand)
        Ast::StrLit.new( type_of( operand ).to_s )
      end

      #--------------------------------------------------------------------------

      private def type_of( expr : Ast::Expr ) : Types::Type
        expr.type || Types::Type.new( Types::EType::Int32 )
      end

    end


  end
end
