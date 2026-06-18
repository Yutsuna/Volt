module Volt
  module Ast


    class Program < Node

      getter externs   : Array(ExternDef)
      getter defs      : Array(Def)
      getter top_level : Array(Node)

      def initialize
        @externs   = [] of ExternDef
        @defs      = [] of Def
        @top_level = [] of Node
      end

    end


  end
end
