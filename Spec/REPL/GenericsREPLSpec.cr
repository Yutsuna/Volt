require "spec"
require "../../Source/Volt/__all__"


module Volt::Spec


  describe "REPL - Generics (end-to-end execution)" do

    it "executes an explicit `Pair[String, Int64]` instantiation" do
      session = REPL::REPLSession.new
      session.evaluate( <<-VOLT ).ok?.should be_true
        class Pair[T, U]
          def initialize( @first : T, @second : U )
          end
          def first -> T
            @first
          end
          def second -> U
            @second
          end
        end
        VOLT

      session.evaluate( %(p = Pair[String, Int64].new( "version", 41 )) ).ok?.should be_true
      session.evaluate( "p.second + 1" ).value.not_nil!.as_i.should eq( 42 )
    end

    it "executes an inferred instantiation defined in an earlier input" do
      session = REPL::REPLSession.new
      session.evaluate( <<-VOLT ).ok?.should be_true
        class Pair[T, U]
          def initialize( @first : T, @second : U )
          end
          def second -> U
            @second
          end
        end
        VOLT

      session.evaluate( %(q = Pair.new( "volt", 2026 )) ).ok?.should be_true
      session.evaluate( "q.second" ).value.not_nil!.as_i.should eq( 2026 )
    end

    it "keeps two instantiations of one template independent at runtime" do
      session = REPL::REPLSession.new
      session.evaluate( <<-VOLT ).value.not_nil!.as_i.should eq( 30 )
        class Box[T]
          def initialize( @value : T )
          end
          def value -> T
            @value
          end
        end

        a = Box[Int64].new( 10 )
        b = Box[Int64].new( 20 )
        a.value + b.value
        VOLT
    end

    it "executes an inferred generic function call" do
      session = REPL::REPLSession.new
      session.evaluate( <<-VOLT ).ok?.should be_true
        def identity( value : T ) -> T forall T
          value
        end
        VOLT

      session.evaluate( "identity( 42 )" ).value.not_nil!.as_i.should eq( 42 )
    end

    it "executes an explicit generic function instantiation" do
      session = REPL::REPLSession.new
      session.evaluate( <<-VOLT ).value.not_nil!.as_i.should eq( 7 )
        def identity( value : T ) -> T forall T
          value
        end
        identity[Int64]( 7 )
        VOLT
    end

    it "monomorphizes one generic function for several types" do
      session = REPL::REPLSession.new
      session.evaluate( <<-VOLT ).value.not_nil!.as_i.should eq( 3 )
        def second( a : T, b : T ) -> T forall T
          b
        end

        s = second( "x", "y" )
        n = second( 1, 3 )
        n
        VOLT
    end

    it "executes a generic pointer swap (`T*` unification)" do
      session = REPL::REPLSession.new
      session.evaluate( <<-VOLT ).value.not_nil!.as_i.should eq( 21 )
        def swap( ptr_a : T*, ptr_b : T* ) -> Nil forall T
          temp = *ptr_a
          *ptr_a = *ptr_b
          *ptr_b = temp
        end

        x = 1
        y = 2
        swap( &x, &y )
        x * 10 + y
        VOLT
    end

    it "runs RAII finalize on an instantiated generic class" do
      session = REPL::REPLSession.new
      result = session.evaluate( <<-VOLT )
        class Tracker[T]
          def initialize( @value : T )
          end
          def finalize
          end
          def value -> T
            @value
          end
        end

        def use_tracker( n : Int64 ) -> Int64
          t = Tracker[Int64].new( n )
          t.value
        end

        use_tracker( 5 ) + use_tracker( 6 )
        VOLT
      result.ok?.should be_true
      result.value.not_nil!.as_i.should eq( 11 )
    end

    it "reuses a persisted instantiation without re-declaring it" do
      session = REPL::REPLSession.new
      session.evaluate( <<-VOLT ).ok?.should be_true
        class Box[T]
          def initialize( @value : T )
          end
          def value -> T
            @value
          end
        end
        VOLT

      session.evaluate( "a = Box[Int64].new( 1 )" ).ok?.should be_true
      session.evaluate( "b = Box[Int64].new( 2 )" ).ok?.should be_true
      session.evaluate( "a.value + b.value" ).value.not_nil!.as_i.should eq( 3 )
    end

    it "reports generic arity errors without corrupting the session" do
      session = REPL::REPLSession.new
      session.evaluate( <<-VOLT ).ok?.should be_true
        class Box[T]
          def initialize( @value : T )
          end
          def value -> T
            @value
          end
        end
        VOLT

      session.evaluate( "bad = Box[Int64, Int64].new( 1 )" ).ok?.should be_false
      session.evaluate( "good = Box[Int64].new( 9 )" ).ok?.should be_true
      session.evaluate( "good.value" ).value.not_nil!.as_i.should eq( 9 )
    end
  end


end
