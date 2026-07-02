require "spec"
require "../../Source/Volt/__all__"


module Volt::Spec


  private def self.slot( name : String, type : Volt::Frontend::Type ) : Volt::Frontend::FieldSlot
    Volt::Frontend::TypeLayout.slot( name, type )
  end


  describe "Volt::Frontend::Type : width & alignment" do

    it "reports natural byte sizes for primitives" do
      Volt::Frontend::Type::INT8.byte_size.should eq( 1 )
      Volt::Frontend::Type::BOOL.byte_size.should eq( 1 )
      Volt::Frontend::Type::INT16.byte_size.should eq( 2 )
      Volt::Frontend::Type::INT32.byte_size.should eq( 4 )
      Volt::Frontend::Type::FLOAT32.byte_size.should eq( 4 )
      Volt::Frontend::Type::INT.byte_size.should eq( 8 )
      Volt::Frontend::Type::FLOAT64.byte_size.should eq( 8 )
      Volt::Frontend::Type::STR.byte_size.should eq( 8 )
    end

    it "treats Float, Float32 and Float64 as floats" do
      Volt::Frontend::Type::FLOAT.float?.should be_true
      Volt::Frontend::Type::FLOAT32.float?.should be_true
      Volt::Frontend::Type::FLOAT64.float?.should be_true
      Volt::Frontend::Type::INT.float?.should be_false
    end

    it "resolves Float32 / Float64 annotations to distinct kinds" do
      f32 = Volt::Frontend::SimpleType.new( "Float32", Volt::Frontend::Span.new( "x", 1, 1, 0 ) )
      f64 = Volt::Frontend::SimpleType.new( "Float64", Volt::Frontend::Span.new( "x", 1, 1, 0 ) )
      Volt::Frontend::Type.from_annotation( f32 ).should eq( Volt::Frontend::Type::FLOAT32 )
      Volt::Frontend::Type.from_annotation( f64 ).should eq( Volt::Frontend::Type::FLOAT64 )
    end

    it "flags heap-reference kinds but not struct values" do
      Volt::Frontend::Type::STR.reference?.should be_true
      Volt::Frontend::NominalType.object( "Device" ).reference?.should be_true
      Volt::Frontend::NominalType.struct( "Point" ).reference?.should be_false
      Volt::Frontend::Type::INT.reference?.should be_false
    end

  end


  describe "Volt::Frontend::TypeLayout : C-ABI packing" do

    it "packs scalars with natural alignment and tail padding" do
      layout = Volt::Frontend::TypeLayout.pack( [
        slot( "a", Volt::Frontend::Type::INT8 ),
        slot( "b", Volt::Frontend::Type::INT ),
        slot( "c", Volt::Frontend::Type::INT8 ),
      ] )

      layout.field?( "a" ).not_nil!.offset.should eq( 0 )
      layout.field?( "b" ).not_nil!.offset.should eq( 8 )
      layout.field?( "c" ).not_nil!.offset.should eq( 16 )
      layout.align.should eq( 8 )
      layout.total_size.should eq( 24 )
    end

    it "aligns a 4-byte field after a 1-byte field" do
      layout = Volt::Frontend::TypeLayout.pack( [
        slot( "flag",  Volt::Frontend::Type::BOOL ),
        slot( "count", Volt::Frontend::Type::INT32 ),
      ] )

      layout.field?( "flag" ).not_nil!.offset.should eq( 0 )
      layout.field?( "count" ).not_nil!.offset.should eq( 4 )
      layout.align.should eq( 4 )
      layout.total_size.should eq( 8 )
    end

    it "produces a zero-size, 1-aligned layout for an empty type" do
      layout = Volt::Frontend::TypeLayout.pack( [] of Volt::Frontend::FieldSlot )
      layout.total_size.should eq( 0 )
      layout.align.should eq( 1 )
      layout.fields.should be_empty
    end

    it "shares the base prefix so inherited fields keep their offsets" do
      base = Volt::Frontend::TypeLayout.pack( [
        slot( "name", Volt::Frontend::Type::STR ),
      ] )
      derived = Volt::Frontend::TypeLayout.pack( [
        slot( "capacity", Volt::Frontend::Type::INT ),
      ], base: base )

      derived.field?( "name" ).not_nil!.offset.should eq( 0 )
      derived.field?( "capacity" ).not_nil!.offset.should eq( 8 )
      derived.total_size.should eq( 16 )
      base.field?( "capacity" ).should be_nil
    end

    it "marks reference fields for deep-drop" do
      layout = Volt::Frontend::TypeLayout.pack( [
        slot( "id",   Volt::Frontend::Type::INT ),
        slot( "conn", Volt::Frontend::NominalType.object( "DatabaseConnection" ) ),
      ] )
      layout.field?( "id" ).not_nil!.is_ref.should be_false
      layout.field?( "conn" ).not_nil!.is_ref.should be_true
    end

  end


end
