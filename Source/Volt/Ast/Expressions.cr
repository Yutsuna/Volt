module Volt
  module Ast


    class VarRef < Expr

      getter name : String
      property slot : String = ""

      def initialize ( @name : String )
      end

    end


    class Assign < Expr

      getter name          : String
      property value       : Expr
      getter declared_type : Types::Type?
      property slot        : String = ""

      def initialize ( @name : String, @value : Expr, @declared_type : Types::Type? = nil )
      end

    end


    class BinaryOp < Expr

      getter op    : String
      property left  : Expr
      property right : Expr

      def initialize ( @left : Expr, @op : String, @right : Expr )
      end

    end


    class UnaryOp < Expr

      getter op       : String
      property operand : Expr

      def initialize ( @op : String, @operand : Expr )
      end

    end


    class Ternary < Expr

      property condition : Expr
      property then_expr : Expr
      property else_expr : Expr

      def initialize ( @condition : Expr, @then_expr : Expr, @else_expr : Expr )
      end

    end


    class Call < Expr

      getter name    : String
      property args   : Array(Expr)

      def initialize ( @name : String, @args : Array(Expr) )
      end

    end


    class TypeOf < Expr

      property operand : Expr

      def initialize ( @operand : Expr )
      end

    end


    class PointerOf < Expr

      property operand : Expr

      def initialize ( @operand : Expr )
      end

    end


    class Cast < Expr

      property operand : Expr

      def initialize ( @operand : Expr, @type : Types::Type )
      end

    end


  end
end
