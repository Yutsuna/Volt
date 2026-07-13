require "spec"
require "../../Source/Volt/__all__"


lib LibC
  fun dup(fd : Int32) : Int32
  fun dup2(oldfd : Int32, newfd : Int32) : Int32
  fun fflush(stream : Void*) : Int32
end


module Volt::Spec


  describe "REPL::REPLSession" do

    it "Simple expressions: evaluate(\"2 + 3\") returns value 5" do
      session = REPL::REPLSession.new
      result = session.evaluate( "2 + 3" )
      result.ok?.should be_true
      result.value.should_not be_nil
      result.value.not_nil!.as_i.should eq( 5 )
    end

    it "Persistent functions: defining a function and then calling it in the next evaluate" do
      session = REPL::REPLSession.new
      r1 = session.evaluate( "def test_fn -> Int64; 99; end" )
      r1.ok?.should be_true

      r2 = session.evaluate( "test_fn()" )
      r2.ok?.should be_true
      r2.value.should_not be_nil
      r2.value.not_nil!.as_i.should eq( 99 )
    end

    it "Function redefinition: a later evaluate can redefine a function and calls use the new body" do
      session = REPL::REPLSession.new
      session.evaluate( "def redef_fn -> Int64; 1; end" ).ok?.should be_true
      session.evaluate( "redef_fn()" ).value.not_nil!.as_i.should eq( 1 )

      session.evaluate( "def redef_fn -> Int64; 100; end" ).ok?.should be_true
      session.evaluate( "redef_fn()" ).value.not_nil!.as_i.should eq( 100 )
    end

    it "Duplicate definition within a single input still errors" do
      session = REPL::REPLSession.new
      res = session.evaluate( "def dup_fn -> Int64; 1; end\ndef dup_fn -> Int64; 2; end" )
      res.ok?.should be_false
    end

    it "Persistent variables: evaluate(\"x = 42\") and then calling evaluate(\"x + 1\") returns 43" do
      session = REPL::REPLSession.new
      r1 = session.evaluate( "x = 42" )
      r1.ok?.should be_true

      r2 = session.evaluate( "x + 1" )
      r2.ok?.should be_true
      r2.value.should_not be_nil
      r2.value.not_nil!.as_i.should eq( 43 )
    end

    it "Single side-effects: calling puts(\"PASS\") doesn't replay when subsequent code is evaluated" do
      session = REPL::REPLSession.new
      session.evaluate( "@[External(\"libc\")] def puts( str : String ) -> Int32" )

      # Save stdout FD 1
      old_stdout_fd = LibC.dup(1)

      r, w = IO.pipe
      # Redirect FD 1 to pipe write-end
      LibC.dup2(w.fd, 1)

      begin
        session.evaluate( "puts(\"PASS\")" )
        LibC.fflush(nil)

        session.evaluate( "2 + 3" )
        LibC.fflush(nil)
      ensure
        # Restore FD 1
        LibC.dup2(old_stdout_fd, 1)
        w.close
      end

      output = r.gets_to_end
      output.scan( "PASS" ).size.should eq( 1 )
    end

    it "load_source variables: load_source defines a variable that is accessible in subsequent evaluations" do
      session = REPL::REPLSession.new
      res1 = session.load_source( "val_from_file = 999", "file.vl" )
      res1.ok?.should be_true

      res2 = session.evaluate( "val_from_file + 1" )
      res2.ok?.should be_true
      res2.value.should_not be_nil
      res2.value.not_nil!.as_i.should eq( 1000 )
    end

    it "Session clear: clear resets the state" do
      session = REPL::REPLSession.new
      session.evaluate( "x = 42" )
      session.clear

      # x should no longer be defined, so evaluate("x") should fail
      res = session.evaluate( "x" )
      res.ok?.should be_false
    end

    it "Empty / whitespace inputs are no-ops and succeed" do
      session = REPL::REPLSession.new
      res1 = session.evaluate( "" )
      res1.ok?.should be_true
      res1.value.should_not be_nil
      res1.value.not_nil!.is_nil?.should be_true

      res2 = session.evaluate( "   \n  " )
      res2.ok?.should be_true
      res2.value.should_not be_nil
      res2.value.not_nil!.is_nil?.should be_true
    end

    it "Recover from compilation errors cleanly without corrupting state" do
      session = REPL::REPLSession.new
      session.evaluate( "x = 42" ).ok?.should be_true

      # Invalid syntax
      res1 = session.evaluate( "y = 10 +" )
      res1.ok?.should be_false

      # Verify state was not corrupted and x is still accessible
      res2 = session.evaluate( "x + 2" )
      res2.ok?.should be_true
      res2.value.should_not be_nil
      res2.value.not_nil!.as_i.should eq( 44 )
    end

    it "Recover from runtime exceptions cleanly without desynchronizing state" do
      session = REPL::REPLSession.new
      session.evaluate( "x = 42" ).ok?.should be_true

      # Division by zero runtime exception
      res1 = session.evaluate( "y = x / 0" )
      # It handles the error and returns nil value
      res1.ok?.should be_true
      res1.value.should be_nil

      # Verify that state was not desynchronized and we can still evaluate new expressions
      res2 = session.evaluate( "x + 3" )
      res2.ok?.should be_true
      res2.value.should_not be_nil
      res2.value.not_nil!.as_i.should eq( 45 )
    end

    it "Cross-evaluation class inheritance and polymorphism" do
      session = REPL::REPLSession.new
      r1 = session.evaluate( "class Parent; def initialize(); end; def value -> Int64; 10; end; end" )
      r1.ok?.should be_true

      r2 = session.evaluate( "class Child < Parent; def initialize(); end; def value -> Int64; 20; end; end" )
      r2.ok?.should be_true

      r3 = session.evaluate( "obj : Parent = Child.new()" )
      r3.ok?.should be_true

      r4 = session.evaluate( "obj.value()" )
      r4.ok?.should be_true
      r4.value.should_not be_nil
      r4.value.not_nil!.as_i.should eq( 20 )
    end

    it "Variable type incompatible reassignment fails typecheck" do
      session = REPL::REPLSession.new
      session.evaluate( "x = 42" ).ok?.should be_true
      res = session.evaluate( "x = \"hello\"" )
      res.ok?.should be_false
    end

    it "Pointer primitive, address, malloc, value, and free in REPL" do
      session = REPL::REPLSession.new
      
      r1 = session.evaluate( "a = Pointer[Int].malloc 2" )
      r1.ok?.should be_true
      r1.value.should_not be_nil
      r1.value.not_nil!.tag.should eq( Volt::IR::Value::Tag::Int )
      
      r2 = session.evaluate( "a.null?" )
      r2.ok?.should be_true
      r2.value.not_nil!.as_bool.should be_false
      
      r3 = session.evaluate( "*a = 42" )
      r3.ok?.should be_true
      
      r4 = session.evaluate( "a.value" )
      r4.ok?.should be_true
      r4.value.not_nil!.as_i.should eq( 42 )
      
      r5 = session.evaluate( "*(a + 1) = 100" )
      r5.ok?.should be_true
      
      r6 = session.evaluate( "(a + 1).value" )
      r6.ok?.should be_true
      r6.value.not_nil!.as_i.should eq( 100 )

      r7 = session.evaluate( "a.free" )
      r7.ok?.should be_true
    end

  end


end
