module Volt
  module Sema


    module Rules::TypeChecking

      COMPARISONS = ["==", "!=", "<", ">", "<=", ">="]
      LOGICALS    = ["&&", "||"]
      ARITHMETIC  = ["+", "-", "*", "/", "%"]

      #--------------------------------------------------------------------------

      private def analyze_binary ( expr : Ast::BinaryOp ) : Ast::Expr
        expr.left  = analyze_expr( expr.left )
        expr.right = analyze_expr( expr.right )
        lt = type_of( expr.left )
        rt = type_of( expr.right )

        if COMPARISONS.includes?( expr.op )
          promote_operands( expr, lt, rt )
          expr.type = Types::Type.new( Types::EType::Bool )
        elsif LOGICALS.includes?( expr.op )
          expr.type = Types::Type.new( Types::EType::Bool )
        elsif ARITHMETIC.includes?( expr.op ) && ( lt.float? || rt.float? )
          ftype = promote_operands( expr, lt, rt )
          expr.type = ftype
        else
          expr.type = lt
        end
        expr
      end

      #--------------------------------------------------------------------------

      private def analyze_unary ( expr : Ast::UnaryOp ) : Ast::Expr
        expr.operand = analyze_expr( expr.operand )
        expr.type =
          case expr.op
          when "!" then Types::Type.new( Types::EType::Bool )
          else          type_of( expr.operand )
          end
        expr
      end

      #--------------------------------------------------------------------------

      private def analyze_ternary ( expr : Ast::Ternary ) : Ast::Expr
        expr.condition = analyze_expr( expr.condition )
        expr.then_expr = analyze_expr( expr.then_expr )
        expr.else_expr = analyze_expr( expr.else_expr )
        expr.type = type_of( expr.then_expr )
        expr
      end

      #--------------------------------------------------------------------------

      private def promote_operands ( expr : Ast::BinaryOp, lt : Types::Type, rt : Types::Type ) : Types::Type
        return lt unless lt.float? || rt.float?

        if lt.float? && rt.float?
          ftype = (lt.base == Types::EType::Float64 || rt.base == Types::EType::Float64) ?
                    Types::Type.new(Types::EType::Float64) : lt
        else
          ftype = lt.float? ? lt : rt
        end

        expr.left  = wrap_cast( expr.left, ftype )  unless lt == ftype
        expr.right = wrap_cast( expr.right, ftype ) unless rt == ftype
        ftype
      end

      private def wrap_cast ( operand : Ast::Expr, target : Types::Type ) : Ast::Expr
        Ast::Cast.new( operand, target )
      end

      private def check_assignment_types(expected : Types::Type, got : Types::Type, line : Int32, col : Int32) : Nil
        if expected != got
          @reporter.error( "type mismatch: cannot reassign '#{expected}' with value of type '#{got}'", line, col )
        end
      end

      #--------------------------------------------------------------------------

    end


  end
end
