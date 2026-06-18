module Volt
  module Ast


    abstract class Node

      include MDumpable

      property line : Int32 = 0
      property col  : Int32 = 0

    end


    abstract class Expr < Node

      property type : Types::Type? = nil

    end


    abstract class Stmt < Node
    end


  end
end
