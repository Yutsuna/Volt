module Volt
  module Ast


    class Param < Node

      getter name  : String
      getter ptype : Types::Type

      def initialize ( @name : String, @ptype : Types::Type )
      end

    end


    class ExternDef < Node

      getter name        : String
      getter params      : Array(Param)
      getter return_type : Types::Type

      def initialize ( @name : String, @params : Array(Param), @return_type : Types::Type )
      end

    end


    class Def < Node

      getter name        : String
      getter params      : Array(Param)
      getter return_type : Types::Type
      property body      : Array(Node)

      def initialize ( @name : String, @params : Array(Param), @return_type : Types::Type, @body : Array(Node) )
      end

    end


    class Return < Stmt

      property value : Expr?

      def initialize ( @value : Expr? = nil )
      end

    end


    class If < Stmt

      property condition : Expr
      property then_body : Array(Node)
      property else_body : Array(Node)?

      def initialize ( @condition : Expr, @then_body : Array(Node), @else_body : Array(Node)? = nil )
      end

    end


    class While < Stmt

      property condition : Expr
      property body      : Array(Node)
      getter is_until    : Bool

      def initialize ( @condition : Expr, @body : Array(Node), @is_until : Bool = false )
      end

    end


    class ExprStmt < Stmt

      property expr : Expr

      def initialize ( @expr : Expr )
      end

    end


  end
end
