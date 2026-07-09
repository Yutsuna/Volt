require "spec"
require "../../Source/Volt/__all__"


module Volt::Spec


  describe "Volt::Frontend::Type : primitives" do

    it "identifies signed integer types" do
      Volt::Frontend::Type::INT8.signed?.should be_true
      Volt::Frontend::Type::INT16.signed?.should be_true
      Volt::Frontend::Type::INT32.signed?.should be_true
      Volt::Frontend::Type::INT64.signed?.should be_true
      Volt::Frontend::Type::INT.signed?.should be_true
      Volt::Frontend::Type::UINT8.signed?.should be_false
      Volt::Frontend::Type::FLOAT32.signed?.should be_false
    end

    it "identifies unsigned integer types" do
      Volt::Frontend::Type::UINT8.unsigned?.should be_true
      Volt::Frontend::Type::UINT16.unsigned?.should be_true
      Volt::Frontend::Type::UINT32.unsigned?.should be_true
      Volt::Frontend::Type::UINT64.unsigned?.should be_true
      Volt::Frontend::Type::UINT.unsigned?.should be_true
      Volt::Frontend::Type::INT8.unsigned?.should be_false
      Volt::Frontend::Type::BOOL.unsigned?.should be_false
    end

    it "identifies integer types" do
      Volt::Frontend::Type::INT8.integer?.should be_true
      Volt::Frontend::Type::INT16.integer?.should be_true
      Volt::Frontend::Type::INT32.integer?.should be_true
      Volt::Frontend::Type::INT64.integer?.should be_true
      Volt::Frontend::Type::INT.integer?.should be_true
      Volt::Frontend::Type::UINT8.integer?.should be_true
      Volt::Frontend::Type::UINT16.integer?.should be_true
      Volt::Frontend::Type::UINT32.integer?.should be_true
      Volt::Frontend::Type::UINT64.integer?.should be_true
      Volt::Frontend::Type::UINT.integer?.should be_true
      Volt::Frontend::Type::FLOAT32.integer?.should be_false
      Volt::Frontend::Type::FLOAT64.integer?.should be_false
      Volt::Frontend::Type::BOOL.integer?.should be_false
    end

    it "identifies float types" do
      Volt::Frontend::Type::FLOAT.float?.should be_true
      Volt::Frontend::Type::FLOAT32.float?.should be_true
      Volt::Frontend::Type::FLOAT64.float?.should be_true
      Volt::Frontend::Type::INT64.float?.should be_false
      Volt::Frontend::Type::BOOL.float?.should be_false
    end

    it "identifies numeric types" do
      Volt::Frontend::Type::INT8.numeric?.should be_true
      Volt::Frontend::Type::UINT64.numeric?.should be_true
      Volt::Frontend::Type::FLOAT32.numeric?.should be_true
      Volt::Frontend::Type::FLOAT64.numeric?.should be_true
      Volt::Frontend::Type::BOOL.numeric?.should be_false
      Volt::Frontend::Type::STR.numeric?.should be_false
    end

    it "reports correct byte sizes for all integer types" do
      Volt::Frontend::Type::INT8.byte_size.should eq( 1 )
      Volt::Frontend::Type::INT16.byte_size.should eq( 2 )
      Volt::Frontend::Type::INT32.byte_size.should eq( 4 )
      Volt::Frontend::Type::INT64.byte_size.should eq( 8 )
      Volt::Frontend::Type::INT.byte_size.should eq( 8 )
      Volt::Frontend::Type::UINT8.byte_size.should eq( 1 )
      Volt::Frontend::Type::UINT16.byte_size.should eq( 2 )
      Volt::Frontend::Type::UINT32.byte_size.should eq( 4 )
      Volt::Frontend::Type::UINT64.byte_size.should eq( 8 )
      Volt::Frontend::Type::UINT.byte_size.should eq( 8 )
    end

    it "reports correct byte sizes for float types" do
      Volt::Frontend::Type::FLOAT32.byte_size.should eq( 4 )
      Volt::Frontend::Type::FLOAT64.byte_size.should eq( 8 )
      Volt::Frontend::Type::FLOAT.byte_size.should eq( 8 )
    end

    it "reports correct byte sizes for Bool and Void" do
      Volt::Frontend::Type::BOOL.byte_size.should eq( 1 )
      Volt::Frontend::Type::NIL.byte_size.should eq( 8 )
    end

    it "reports correct int bit widths" do
      Volt::Frontend::Type::INT8.int_bit_width.should eq( 8 )
      Volt::Frontend::Type::INT16.int_bit_width.should eq( 16 )
      Volt::Frontend::Type::INT32.int_bit_width.should eq( 32 )
      Volt::Frontend::Type::INT64.int_bit_width.should eq( 64 )
      Volt::Frontend::Type::INT.int_bit_width.should eq( 64 )
      Volt::Frontend::Type::UINT8.int_bit_width.should eq( 8 )
      Volt::Frontend::Type::UINT16.int_bit_width.should eq( 16 )
      Volt::Frontend::Type::UINT32.int_bit_width.should eq( 32 )
      Volt::Frontend::Type::UINT64.int_bit_width.should eq( 64 )
      Volt::Frontend::Type::UINT.int_bit_width.should eq( 64 )
      Volt::Frontend::Type::FLOAT32.int_bit_width.should eq( 0 )
      Volt::Frontend::Type::BOOL.int_bit_width.should eq( 0 )
    end

    it "resolves all integer type annotations correctly" do
      types = {
        "Int8"    => Volt::Frontend::Type::INT8,
        "Int16"   => Volt::Frontend::Type::INT16,
        "Int32"   => Volt::Frontend::Type::INT32,
        "Int64"   => Volt::Frontend::Type::INT64,
        "UInt8"   => Volt::Frontend::Type::UINT8,
        "UInt16"  => Volt::Frontend::Type::UINT16,
        "UInt32"  => Volt::Frontend::Type::UINT32,
        "UInt64"  => Volt::Frontend::Type::UINT64,
      }
      types.each do |name, expected|
        node = Volt::Frontend::SimpleType.new( name, Volt::Frontend::Span.new( "test", 1, 1, 0 ) )
        Volt::Frontend::Type.from_annotation( node ).should eq( expected )
      end
    end

    it "resolves all float type annotations correctly" do
      types = {
        "Float32" => Volt::Frontend::Type::FLOAT32,
        "Float64" => Volt::Frontend::Type::FLOAT64,
        "Float"   => Volt::Frontend::Type::FLOAT,
      }
      types.each do |name, expected|
        node = Volt::Frontend::SimpleType.new( name, Volt::Frontend::Span.new( "test", 1, 1, 0 ) )
        Volt::Frontend::Type.from_annotation( node ).should eq( expected )
      end
    end

    it "resolves Bool and Void type annotations correctly" do
      bool_node = Volt::Frontend::SimpleType.new( "Bool", Volt::Frontend::Span.new( "test", 1, 1, 0 ) )
      Volt::Frontend::Type.from_annotation( bool_node ).should eq( Volt::Frontend::Type::BOOL )

      void_node = Volt::Frontend::SimpleType.new( "Void", Volt::Frontend::Span.new( "test", 1, 1, 0 ) )
      Volt::Frontend::Type.from_annotation( void_node ).should eq( Volt::Frontend::Type::NIL )

      nil_node = Volt::Frontend::SimpleType.new( "Nil", Volt::Frontend::Span.new( "test", 1, 1, 0 ) )
      Volt::Frontend::Type.from_annotation( nil_node ).should eq( Volt::Frontend::Type::NIL )
    end

    it "returns nil for unknown type annotations" do
      node = Volt::Frontend::SimpleType.new( "UnknownType", Volt::Frontend::Span.new( "test", 1, 1, 0 ) )
      Volt::Frontend::Type.from_annotation( node ).should be_nil
    end

    it "treats all integer types as reference? false" do
      Volt::Frontend::Type::INT8.reference?.should be_false
      Volt::Frontend::Type::INT16.reference?.should be_false
      Volt::Frontend::Type::INT32.reference?.should be_false
      Volt::Frontend::Type::INT64.reference?.should be_false
      Volt::Frontend::Type::UINT8.reference?.should be_false
      Volt::Frontend::Type::UINT16.reference?.should be_false
      Volt::Frontend::Type::UINT32.reference?.should be_false
      Volt::Frontend::Type::UINT64.reference?.should be_false
    end

    it "treats all float types as reference? false" do
      Volt::Frontend::Type::FLOAT32.reference?.should be_false
      Volt::Frontend::Type::FLOAT64.reference?.should be_false
    end

    it "treats Bool as reference? false" do
      Volt::Frontend::Type::BOOL.reference?.should be_false
    end

    it "treats Nil as reference? false" do
      Volt::Frontend::Type::NIL.reference?.should be_false
    end

    it "returns correct string representation for all integer types" do
      Volt::Frontend::Type::INT8.to_s.should eq( "Int8" )
      Volt::Frontend::Type::INT16.to_s.should eq( "Int16" )
      Volt::Frontend::Type::INT32.to_s.should eq( "Int32" )
      Volt::Frontend::Type::INT64.to_s.should eq( "Int64" )
      Volt::Frontend::Type::INT.to_s.should eq( "Int" )
      Volt::Frontend::Type::UINT8.to_s.should eq( "UInt8" )
      Volt::Frontend::Type::UINT16.to_s.should eq( "UInt16" )
      Volt::Frontend::Type::UINT32.to_s.should eq( "UInt32" )
      Volt::Frontend::Type::UINT64.to_s.should eq( "UInt64" )
      Volt::Frontend::Type::UINT.to_s.should eq( "UInt" )
    end

    it "returns correct string representation for float types" do
      Volt::Frontend::Type::FLOAT32.to_s.should eq( "Float32" )
      Volt::Frontend::Type::FLOAT64.to_s.should eq( "Float64" )
      Volt::Frontend::Type::FLOAT.to_s.should eq( "Float" )
    end

    it "returns correct string representation for Bool and Nil" do
      Volt::Frontend::Type::BOOL.to_s.should eq( "Bool" )
      Volt::Frontend::Type::NIL.to_s.should eq( "Nil" )
    end

  end


  describe "Volt::Frontend::Type : pointer" do

    it "creates pointer types" do
      int_ptr = Volt::Frontend::Type.pointer( Volt::Frontend::Type::INT64 )
      int_ptr.pointer?.should be_true
      int_ptr.kind.pointer?.should be_true
    end

    it "extracts pointee type from pointer" do
      int_ptr = Volt::Frontend::Type.pointer( Volt::Frontend::Type::INT64 )
      int_ptr.pointee.should eq( Volt::Frontend::Type::INT64 )

      float_ptr = Volt::Frontend::Type.pointer( Volt::Frontend::Type::FLOAT32 )
      float_ptr.pointee.should eq( Volt::Frontend::Type::FLOAT32 )
    end

    it "identifies pointer types correctly" do
      int_ptr = Volt::Frontend::Type.pointer( Volt::Frontend::Type::INT64 )
      int_ptr.pointer?.should be_true
      Volt::Frontend::Type::INT64.pointer?.should be_false
      Volt::Frontend::Type::FLOAT64.pointer?.should be_false
      Volt::Frontend::Type::BOOL.pointer?.should be_false
    end

    it "resolves pointer type annotations" do
      node = Volt::Frontend::PointerType.new(
        Volt::Frontend::SimpleType.new( "Int64", Volt::Frontend::Span.new( "test", 1, 1, 0 ) ),
        Volt::Frontend::Span.new( "test", 1, 1, 0 )
      )
      result = Volt::Frontend::Type.from_annotation( node )
      result.not_nil!.pointer?.should be_true
      result.not_nil!.pointee.should eq( Volt::Frontend::Type::INT64 )
    end

    it "reports correct byte size for pointer types" do
      int_ptr = Volt::Frontend::Type.pointer( Volt::Frontend::Type::INT64 )
      int_ptr.byte_size.should eq( 8 )

      float_ptr = Volt::Frontend::Type.pointer( Volt::Frontend::Type::FLOAT32 )
      float_ptr.byte_size.should eq( 8 )
    end

    it "returns correct string representation for pointer types" do
      int_ptr = Volt::Frontend::Type.pointer( Volt::Frontend::Type::INT64 )
      int_ptr.to_s.should eq( "*Int64" )

      uint8_ptr = Volt::Frontend::Type.pointer( Volt::Frontend::Type::UINT8 )
      uint8_ptr.to_s.should eq( "*UInt8" )

      float64_ptr = Volt::Frontend::Type.pointer( Volt::Frontend::Type::FLOAT64 )
      float64_ptr.to_s.should eq( "*Float64" )
    end

    it "compares pointer types by pointee" do
      int_ptr1 = Volt::Frontend::Type.pointer( Volt::Frontend::Type::INT64 )
      int_ptr2 = Volt::Frontend::Type.pointer( Volt::Frontend::Type::INT64 )
      int_ptr1.should eq( int_ptr2 )

      int_ptr = Volt::Frontend::Type.pointer( Volt::Frontend::Type::INT64 )
      float_ptr = Volt::Frontend::Type.pointer( Volt::Frontend::Type::FLOAT64 )
      int_ptr.should_not eq( float_ptr )
    end

    it "creates nested pointer types" do
      int_ptr_ptr = Volt::Frontend::Type.pointer(
        Volt::Frontend::Type.pointer( Volt::Frontend::Type::INT64 )
      )
      int_ptr_ptr.pointer?.should be_true
      int_ptr_ptr.pointee.pointer?.should be_true
      int_ptr_ptr.pointee.pointee.should eq( Volt::Frontend::Type::INT64 )
      int_ptr_ptr.to_s.should eq( "**Int64" )
    end

    it "creates pointer to all primitive types" do
      primitives = [
        Volt::Frontend::Type::INT8,
        Volt::Frontend::Type::INT16,
        Volt::Frontend::Type::INT32,
        Volt::Frontend::Type::INT64,
        Volt::Frontend::Type::UINT8,
        Volt::Frontend::Type::UINT16,
        Volt::Frontend::Type::UINT32,
        Volt::Frontend::Type::UINT64,
        Volt::Frontend::Type::FLOAT32,
        Volt::Frontend::Type::FLOAT64,
        Volt::Frontend::Type::BOOL,
      ]
      primitives.each do |primitive|
        ptr = Volt::Frontend::Type.pointer( primitive )
        ptr.pointer?.should be_true
        ptr.pointee.should eq( primitive )
        ptr.byte_size.should eq( 8 )
      end
    end

  end


end
