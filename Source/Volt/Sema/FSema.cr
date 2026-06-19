require "./FContext"
require "./Rules/**"

module Volt
  module Sema


    class FSema

      #--------------------------------------------------------------------------

      include Rules::ControlFlow
      include Rules::TypeChecking
      include Rules::Declarations
      include Rules::Expressions

      #--------------------------------------------------------------------------

      def initialize ( @program : Ast::Program, @reporter : Diagnostic::FReporter )
        @externs = {} of String => Ast::ExternDef
        @defs    = {} of String => Ast::Def
        @context = FContext.new
      end

      #--------------------------------------------------------------------------

      def analyze : Nil
        @program.externs.each { |e| @externs[e.name] = e }
        @program.defs.each { |d| @defs[d.name] = d }

        @program.defs.each { |d| analyze_def(d) }
        @program.top_level.each { |node| analyze_node(node) }
      end

    end


  end
end
