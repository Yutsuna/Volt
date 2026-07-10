require "spec"
require "../../Source/Volt/__all__"


module Volt::Spec


  describe "REPL::REPLEvaluationResult" do


    # -------------------------------------------------------------------------
    # ok? - nil diagnostics
    # -------------------------------------------------------------------------

    it "ok? returns true when diagnostics is nil" do
      result = REPL::REPLEvaluationResult.new( nil, nil, false )
      result.ok?.should be_true
    end


    # -------------------------------------------------------------------------
    # ok? - DiagnosticBag with no errors
    # -------------------------------------------------------------------------

    it "ok? returns true when DiagnosticBag is empty" do
      bag = Frontend::DiagnosticBag.new
      result = REPL::REPLEvaluationResult.new( nil, bag, false )
      result.ok?.should be_true
    end

    it "ok? returns true when DiagnosticBag contains only warnings" do
      bag = Frontend::DiagnosticBag.new
      bag << Frontend::Diagnostic.warning( "W001", "unused variable" )
      result = REPL::REPLEvaluationResult.new( nil, bag, false )
      result.ok?.should be_true
    end

    it "ok? returns true when DiagnosticBag contains only notes" do
      bag = Frontend::DiagnosticBag.new
      bag << Frontend::Diagnostic.new( "N001", Frontend::Severity::Note, "informational note" )
      result = REPL::REPLEvaluationResult.new( nil, bag, false )
      result.ok?.should be_true
    end


    # -------------------------------------------------------------------------
    # ok? - DiagnosticBag with errors
    # -------------------------------------------------------------------------

    it "ok? returns false when DiagnosticBag contains one error" do
      bag = Frontend::DiagnosticBag.new
      bag << Frontend::Diagnostic.error( "E001", "undefined variable 'x'" )
      result = REPL::REPLEvaluationResult.new( nil, bag, false )
      result.ok?.should be_false
    end

    it "ok? returns false when DiagnosticBag contains multiple errors" do
      bag = Frontend::DiagnosticBag.new
      bag << Frontend::Diagnostic.error( "E001", "undefined variable 'x'" )
      bag << Frontend::Diagnostic.error( "E002", "type mismatch: expected Int, got String" )
      result = REPL::REPLEvaluationResult.new( nil, bag, false )
      result.ok?.should be_false
    end

    it "ok? returns false when DiagnosticBag contains a mix of error and warning" do
      bag = Frontend::DiagnosticBag.new
      bag << Frontend::Diagnostic.warning( "W001", "shadowed binding" )
      bag << Frontend::Diagnostic.error( "E003", "unresolved identifier" )
      result = REPL::REPLEvaluationResult.new( nil, bag, false )
      result.ok?.should be_false
    end


    # -------------------------------------------------------------------------
    # definition_saved?
    # -------------------------------------------------------------------------

    it "definition_saved? returns false when constructed with false" do
      result = REPL::REPLEvaluationResult.new( nil, nil, false )
      result.definition_saved?.should be_false
    end

    it "definition_saved? returns true when constructed with true" do
      result = REPL::REPLEvaluationResult.new( nil, nil, true )
      result.definition_saved?.should be_true
    end


    # -------------------------------------------------------------------------
    # value getter
    # -------------------------------------------------------------------------

    it "value getter returns nil when no value was provided" do
      result = REPL::REPLEvaluationResult.new( nil, nil, false )
      result.value.should be_nil
    end

    it "value getter returns the integer value that was provided" do
      v = IR::Value.int( 42_i64 )
      result = REPL::REPLEvaluationResult.new( v, nil, false )
      result.value.should_not be_nil
      result.value.not_nil!.as_i.should eq( 42_i64 )
    end


    it "value getter returns the boolean value that was provided" do
      v = IR::Value.bool( true )
      result = REPL::REPLEvaluationResult.new( v, nil, false )
      result.value.not_nil!.as_bool.should be_true
    end

    it "value getter returns a nil-value IR::Value when nil_value was provided" do
      v = IR::Value.nil_value
      result = REPL::REPLEvaluationResult.new( v, nil, false )
      result.value.not_nil!.is_nil?.should be_true
    end


    # -------------------------------------------------------------------------
    # diagnostics getter
    # -------------------------------------------------------------------------

    it "diagnostics getter returns nil when no bag was provided" do
      result = REPL::REPLEvaluationResult.new( nil, nil, false )
      result.diagnostics.should be_nil
    end

    it "diagnostics getter returns the provided DiagnosticBag with correct error count" do
      bag = Frontend::DiagnosticBag.new
      bag << Frontend::Diagnostic.error( "E001", "oops" )
      result = REPL::REPLEvaluationResult.new( nil, bag, false )
      result.diagnostics.should_not be_nil
      result.diagnostics.not_nil!.error_count.should eq( 1 )
    end


  end


end
