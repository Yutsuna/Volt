module Volt
  module Ast


    class IntLit < Expr

      getter value : Int64

      def initialize ( @value : Int64, @type : Types::Type )
      end

    end


    class FloatLit < Expr

      getter value : Float64

      def initialize ( @value : Float64, @type : Types::Type )
      end

    end


    class BoolLit < Expr

      getter value : Bool

      def initialize ( @value : Bool )
        @type = Types::Type.new(Types::EType::Bool)
      end

    end


    class CharLit < Expr

      getter value : UInt8

      def initialize ( @value : UInt8 )
        @type = Types::Type.new(Types::EType::Char)
      end

    end


    class StrLit < Expr

      getter value : String

      def initialize ( @value : String )
        @type = Types::Type.new(Types::EType::UInt8, 1)
      end

    end


    class NilLit < Expr

      def initialize
        @type = Types::Type.new(Types::EType::Nil)
      end

    end


    class ArrayLit < Expr

      property elements : Array(Expr)

      def initialize ( @elements : Array(Expr) )
      end

    end


  end
end
