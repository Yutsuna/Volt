require "spec"
require "../../../Source/Volt/__all__"


private PAIR_TEMPLATE = <<-VOLT
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

private def analyse_generics( source : String ) : Volt::Frontend::TypedProgram
    Volt::Frontend.analyse( Volt::Frontend.parse( source, "generics_sema.vl" ) )
  rescue err : Volt::Frontend::CompilationError
    err.bag.diagnostics.each do |d|
      puts "SEMA DIAGNOSTIC: #{d.code} - #{d.message} at #{d.primary_span}"
    end
    raise err
  end

private def error_codes( source : String ) : Array( String )
    analyse_generics( source )
    [] of String
  rescue err : Volt::Frontend::CompilationError
    err.bag.diagnostics.map( &.code )
  end


  describe "Volt::Frontend - Generics (monomorphization)" do

    it "instantiates an explicit `Pair[String, Int64].new`" do
      tp = analyse_generics( "#{PAIR_TEMPLATE}\np = Pair[String, Int64].new( \"v\", 1 )" )
      tp.types.has_key?( "Pair[String, Int64]" ).should be_true
      tp.types.has_key?( "Pair" ).should be_false
      info = tp.types[ "Pair[String, Int64]" ]
      info.layout.not_nil!.fields.map( &.type.to_s ).should eq( [ "String", "Int64" ] )
      info.methods[ "first" ].ret.to_s.should eq( "String" )
      info.methods[ "second" ].ret.to_s.should eq( "Int64" )
    end

    it "infers type arguments from constructor arguments" do
      tp = analyse_generics( "#{PAIR_TEMPLATE}\np = Pair.new( \"volt\", 2026 )" )
      tp.types.keys.any?( &.starts_with?( "Pair[" ) ).should be_true
    end

    it "keeps two instantiations of one template distinct" do
      tp = analyse_generics( <<-VOLT )
        #{PAIR_TEMPLATE}
        a = Pair[String, Int64].new( "v", 1 )
        b = Pair[Int64, Int64].new( 2, 3 )
        VOLT
      tp.types.has_key?( "Pair[String, Int64]" ).should be_true
      tp.types.has_key?( "Pair[Int64, Int64]" ).should be_true
      tp.types[ "Pair[String, Int64]" ].methods[ "first" ].ret.to_s.should eq( "String" )
      tp.types[ "Pair[Int64, Int64]" ].methods[ "first" ].ret.to_s.should eq( "Int64" )
    end

    it "supports nested generic type arguments" do
      tp = analyse_generics( <<-VOLT )
        #{PAIR_TEMPLATE}
        n = Pair[Pair[Int64, Int64], String].new( Pair[Int64, Int64].new( 1, 2 ), "x" )
        VOLT
      tp.types.has_key?( "Pair[Int64, Int64]" ).should be_true
      tp.types.has_key?( "Pair[Pair[Int64, Int64], String]" ).should be_true
    end

    it "resolves generic references in field annotations of concrete classes" do
      tp = analyse_generics( <<-VOLT )
        #{PAIR_TEMPLATE}
        class Holder
          def initialize( @p : Pair[Int64, Int64] )
          end
        end
        h = Holder.new( Pair[Int64, Int64].new( 1, 2 ) )
        VOLT
      tp.types[ "Holder" ].layout.not_nil!.fields.first.type.to_s.should eq( "Pair[Int64, Int64]" )
    end

    it "resolves generic references in concrete function signatures" do
      tp = analyse_generics( <<-VOLT )
        #{PAIR_TEMPLATE}
        def sum_pair( p : Pair[Int64, Int64] ) -> Int64
          p.first + p.second
        end
        s = sum_pair( Pair[Int64, Int64].new( 1, 2 ) )
        VOLT
      tp.signatures[ "sum_pair" ].params.first.to_s.should eq( "Pair[Int64, Int64]" )
    end

    it "instantiates a generic function per concrete argument type" do
      tp = analyse_generics( <<-VOLT )
        def identity( value : T ) -> T forall T
          value
        end
        a = identity( 42 )
        b = identity( "text" )
        VOLT
      tp.signatures.has_key?( "identity" ).should be_false
      tp.signatures.keys.count( &.starts_with?( "identity[" ) ).should eq( 2 )
      tp.functions.count( &.name.starts_with?( "identity[" ) ).should eq( 2 )
    end

    it "rewrites generic call sites to the mangled name" do
      tp = analyse_generics( <<-VOLT )
        def identity( value : T ) -> T forall T
          value
        end
        a = identity( 42 )
        VOLT
      assign = tp.top_level.first.as( Volt::Frontend::Assign )
      call   = assign.value.as( Volt::Frontend::Call )
      call.callee.as( Volt::Frontend::Ident ).name.should start_with( "identity[" )
    end

    it "unifies a type parameter through a pointer (`T*`)" do
      tp = analyse_generics( <<-VOLT )
        def swap( ptr_a : T*, ptr_b : T* ) -> Nil forall T
          temp = *ptr_a
          *ptr_a = *ptr_b
          *ptr_b = temp
        end
        x = 1
        y = 2
        swap( &x, &y )
        VOLT
      tp.signatures.keys.any?( &.starts_with?( "swap[" ) ).should be_true
    end

    it "supports explicit generic function instantiation `identity[Int64]( 7 )`" do
      tp = analyse_generics( <<-VOLT )
        def identity( value : T ) -> T forall T
          value
        end
        a = identity[Int64]( 7 )
        VOLT
      tp.signatures.has_key?( "identity[Int64]" ).should be_true
    end

    it "rejects a wrong number of type arguments (S0057)" do
      codes = error_codes( "#{PAIR_TEMPLATE}\np = Pair[String].new( \"v\", 1 )" )
      codes.should contain( "S0057" )
    end

    it "rejects a bare generic name without type arguments (S0058)" do
      codes = error_codes( "#{PAIR_TEMPLATE}\np = Pair.first" )
      codes.should contain( "S0058" )
    end

    it "reports an uninferrable type parameter (S0059)" do
      codes = error_codes( <<-VOLT )
        class Empty[T]
          def initialize
          end
        end
        e = Empty.new
        VOLT
      codes.should contain( "S0059" )
    end

    it "rejects an unknown type argument (S0060)" do
      codes = error_codes( "#{PAIR_TEMPLATE}\np = Pair[Bogus, Int64].new( 1, 2 )" )
      codes.should contain( "S0060" )
    end

    it "caps runaway recursive instantiation (S0061)" do
      codes = error_codes( <<-VOLT )
        class Wrap[T]
          def initialize( @w : Wrap[Wrap[T]] )
          end
        end
        def force( w : Wrap[Int64] ) -> Int64
          0
        end
        VOLT
      codes.should contain( "S0061" )
    end

    it "rejects generic methods inside a type (S0063)" do
      codes = error_codes( <<-VOLT )
        class Box
          def initialize
          end
          def convert[T]( value : T ) -> T
            value
          end
        end
        VOLT
      codes.should contain( "S0063" )
    end

    it "type-checks instantiated bodies (bad argument still caught)" do
      codes = error_codes( "#{PAIR_TEMPLATE}\np = Pair[String, Int64].new( 1, 2 )" )
      codes.should contain( "S0022" )
    end
  end


