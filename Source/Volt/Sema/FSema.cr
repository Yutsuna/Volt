require "./Context"
require "./Rules/**"

module Volt
  module Sema

    class FSema

      #--------------------------------------------------------------------------

      include Rules::ControlFlow
      include Rules::TypeChecking
      include Rules::Declarations
      include Rules::Expressions

      COMPARISONS = ["==", "!=", "<", ">", "<=", ">="]
      LOGICALS    = ["&&", "||"]
      ARITHMETIC  = ["+", "-", "*", "/", "%"]

      #--------------------------------------------------------------------------

      def initialize ( @program : Ast::Program, @reporter : Diagnostic::FReporter )
        @externs = {} of String => Ast::ExternDef
        @defs    = {} of String => Ast::Def
        @context = SemaContext.new
      end

      #--------------------------------------------------------------------------

      def analyze : Nil
        @program.externs.each { |e| @externs[e.name] = e }
        @program.defs.each { |d| @defs[d.name] = d }

        @program.defs.each { |d| analyze_def(d) }

        # L'analyse globale s'exécute sur le scope racine globale
        @program.top_level.each { |node| analyze_node(node) }
      end

    end

  end
end
