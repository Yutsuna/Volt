module Volt::REPL


  struct REPLEvaluationResult
    getter value : IR::Value?
    getter diagnostics : Frontend::DiagnosticBag?
    getter? definition_saved : Bool

    def initialize(@value : IR::Value?, @diagnostics : Frontend::DiagnosticBag?, @definition_saved : Bool)
    end

    def ok? : Bool
      @diagnostics.nil? || !@diagnostics.not_nil!.errors?
    end
  end


end
