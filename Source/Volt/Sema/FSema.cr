module Volt
  module Sema


    class FSema

      COMPARISONS = ["==", "!=", "<", ">", "<=", ">="]
      LOGICALS    = ["&&", "||"]
      ARITHMETIC  = ["+", "-", "*", "/", "%"]

      #--------------------------------------------------------------------------

      def initialize ( @program : Ast::Program, @reporter : Diagnostic::FReporter )
        @externs = {} of String => Ast::ExternDef
        @defs    = {} of String => Ast::Def
      end

      #--------------------------------------------------------------------------

      def analyze : Nil
        @program.externs.each { |e| @externs[e.name] = e }
        @program.defs.each { |d| @defs[d.name] = d }

        @program.defs.each { |d| analyze_def(d) }

        top_scope = FScope.new
        @program.top_level.each { |node| analyze_node(node, top_scope) }
      end

      #--------------------------------------------------------------------------

      private def analyze_def ( d : Ast::Def ) : Nil
        scope = FScope.new
        d.params.each { |p| scope.define_param(p.name, p.ptype) }
        d.body.each { |node| analyze_node(node, scope) }

        last = d.body.last?
        if !d.return_type.void? && last.is_a?(Ast::ExprStmt)
          d.body[d.body.size - 1] = Ast::Return.new(last.expr)
        end
      end

      private def analyze_body ( body : Array(Ast::Node), scope : FScope ) : Nil
        body.each { |node| analyze_node(node, scope) }
      end

      private def analyze_node ( node : Ast::Node, scope : FScope ) : Nil
        case node
        when Ast::ExprStmt
          node.expr = analyze_expr(node.expr, scope)
        when Ast::Return
          v = node.value
          node.value = analyze_expr(v, scope) if v
        when Ast::If
          node.condition = analyze_expr(node.condition, scope)
          analyze_body(node.then_body, scope)
          eb = node.else_body
          analyze_body(eb, scope) if eb
        when Ast::While
          node.condition = analyze_expr(node.condition, scope)
          analyze_body(node.body, scope)
        when Ast::Expr
          analyze_expr(node, scope)
        end
      end

      #--------------------------------------------------------------------------

      private def analyze_expr ( expr : Ast::Expr, scope : FScope ) : Ast::Expr
        case expr
        when Ast::IntLit, Ast::FloatLit, Ast::BoolLit, Ast::CharLit,
             Ast::StrLit, Ast::NilLit
          expr
        when Ast::ArrayLit  then analyze_array(expr, scope)
        when Ast::VarRef    then analyze_varref(expr, scope)
        when Ast::Assign    then analyze_assign(expr, scope)
        when Ast::BinaryOp  then analyze_binary(expr, scope)
        when Ast::UnaryOp   then analyze_unary(expr, scope)
        when Ast::Ternary   then analyze_ternary(expr, scope)
        when Ast::Call      then analyze_call(expr, scope)
        when Ast::TypeOf    then analyze_typeof(expr, scope)
        when Ast::PointerOf then analyze_pointerof(expr, scope)
        else                     expr
        end
      end

      private def analyze_array ( expr : Ast::ArrayLit, scope : FScope ) : Ast::Expr
        expr.elements = expr.elements.map { |e| analyze_expr(e, scope) }
        elem = expr.elements.first?
        base = elem ? type_of(elem).base : Types::EType::Int32
        expr.type = Types::Type.new(base, 1)
        expr
      end

      private def analyze_varref ( expr : Ast::VarRef, scope : FScope ) : Ast::Expr
        if found = scope.lookup(expr.name)
          expr.slot = found[0]
          expr.type = found[1]
          return expr
        end

        if zero_arg_callable?(expr.name)
          return analyze_expr(Ast::Call.new(expr.name, [] of Ast::Expr), scope)
        end

        @reporter.error("undefined variable '#{expr.name}'", expr.line, expr.col)
        expr.type = Types::Type.new(Types::EType::Int32)
        expr
      end

      private def analyze_assign ( expr : Ast::Assign, scope : FScope ) : Ast::Expr
        expr.value = analyze_expr(expr.value, scope)
        vtype = expr.declared_type || type_of(expr.value)
        expr.slot = scope.assign(expr.name, vtype)
        expr.type = vtype
        expr
      end

      private def analyze_binary ( expr : Ast::BinaryOp, scope : FScope ) : Ast::Expr
        expr.left  = analyze_expr(expr.left, scope)
        expr.right = analyze_expr(expr.right, scope)
        lt = type_of(expr.left)
        rt = type_of(expr.right)

        if COMPARISONS.includes?(expr.op)
          promote_operands(expr, lt, rt)
          expr.type = Types::Type.new(Types::EType::Bool)
        elsif LOGICALS.includes?(expr.op)
          expr.type = Types::Type.new(Types::EType::Bool)
        elsif ARITHMETIC.includes?(expr.op) && (lt.float? || rt.float?)
          ftype = promote_operands(expr, lt, rt)
          expr.type = ftype
        else
          expr.type = lt
        end
        expr
      end

      private def promote_operands ( expr : Ast::BinaryOp, lt : Types::Type, rt : Types::Type ) : Types::Type
        return lt unless lt.float? || rt.float?
        ftype = lt.float? ? lt : rt
        expr.left  = wrap_cast(expr.left, ftype) unless lt.float?
        expr.right = wrap_cast(expr.right, ftype) unless rt.float?
        ftype
      end

      private def wrap_cast ( operand : Ast::Expr, target : Types::Type ) : Ast::Expr
        cast = Ast::Cast.new(operand, target)
        cast
      end

      private def analyze_unary ( expr : Ast::UnaryOp, scope : FScope ) : Ast::Expr
        expr.operand = analyze_expr(expr.operand, scope)
        expr.type =
          case expr.op
          when "!" then Types::Type.new(Types::EType::Bool)
          else          type_of(expr.operand)
          end
        expr
      end

      private def analyze_ternary ( expr : Ast::Ternary, scope : FScope ) : Ast::Expr
        expr.condition = analyze_expr(expr.condition, scope)
        expr.then_expr = analyze_expr(expr.then_expr, scope)
        expr.else_expr = analyze_expr(expr.else_expr, scope)
        expr.type = type_of(expr.then_expr)
        expr
      end

      private def analyze_call ( expr : Ast::Call, scope : FScope ) : Ast::Expr
        expr.args = expr.args.map { |a| analyze_expr(a, scope) }
        expr.type = call_return_type(expr.name, expr)
        expr
      end

      private def analyze_typeof ( expr : Ast::TypeOf, scope : FScope ) : Ast::Expr
        operand = analyze_expr(expr.operand, scope)
        Ast::StrLit.new(type_of(operand).to_s)
      end

      private def analyze_pointerof ( expr : Ast::PointerOf, scope : FScope ) : Ast::Expr
        expr.operand = analyze_expr(expr.operand, scope)
        expr.type = type_of(expr.operand).to_pointer
        expr
      end

      #--------------------------------------------------------------------------

      private def zero_arg_callable? ( name : String ) : Bool
        if d = @defs[name]?
          return d.params.empty?
        end
        if e = @externs[name]?
          return e.params.empty?
        end
        false
      end

      private def call_return_type ( name : String, call : Ast::Call ) : Types::Type
        if d = @defs[name]?
          return d.return_type
        end
        if e = @externs[name]?
          return e.return_type
        end
        case name
        when "puts" then Types::Type.new(Types::EType::Int32)
        when "exit" then Types::Type.new(Types::EType::Nil)
        else
          @reporter.error("undefined function '#{name}'", call.line, call.col)
          Types::Type.new(Types::EType::Int32)
        end
      end

      private def type_of ( expr : Ast::Expr ) : Types::Type
        expr.type || Types::Type.new(Types::EType::Int32)
      end

    end


  end
end
