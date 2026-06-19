require "./FScope"

module Volt
  module Sema


    class FContext

      property current_scope : FScope
      property current_function : Ast::Def?
      property loop_depth : Int32
      property terminated : Bool

      def initialize
        @urrent_scope = FScope.new
        @current_function = nil
        @loop_depth = 0
        @terminated = false
      end

      def push : FScope
        @current_scope = FScope.new @current_scope
      end

      def pop! : FScope
        if parent = @current_scope.parent
          @current_scope = parent
        else
          raise "Sema FContext: Cannot pop scope, no parent scope"
        end
      end

      def with_scope( & )
        push
        yield
      ensure
        pop!
      end

      def with_loop( & )
        @loop_depth += 1
        yield
      ensure
        @loop_depth -= 1
      end

      def with_function( def_node : Ast::Def, & )
        old_func = @current_function
        old_term = @terminated
        @current_function = func
        @terminated = false
        yield
      ensure
        @current_function = old_func
        @terminated = old_term
      end

    end


  end
end
