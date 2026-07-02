require "spec"
require "../../../Source/Volt/__all__"


module Volt::Spec


  describe "Volt::Frontend::Type" do

    it "initializes with kind, params, and return type" do
      params = [] of Volt::Frontend::Type
      type = Volt::Frontend::Type.new( Volt::Frontend::TypeKind::Int, params, nil )
      type.kind.should eq( Volt::Frontend::TypeKind::Int )
      type.params.should be_empty
      type.ret.should be_nil
    end

    it "creates function type" do
      int_params = [] of Volt::Frontend::Type
      str_params = [] of Volt::Frontend::Type
      int_type = Volt::Frontend::Type.new( Volt::Frontend::TypeKind::Int, int_params, nil )
      str_type = Volt::Frontend::Type.new( Volt::Frontend::TypeKind::Str, str_params, nil )
      func_type = Volt::Frontend::Type.func( [ int_type ], str_type )
      func_type.kind.should eq( Volt::Frontend::TypeKind::Func )
      func_type.params.size.should eq( 1 )
      func_type.params[ 0 ].should eq( int_type )
      func_type.ret.should eq( str_type )
    end

    it "has built-in type constants" do
      Volt::Frontend::Type::INT8.kind.should eq( Volt::Frontend::TypeKind::Int8 )
      Volt::Frontend::Type::INT16.kind.should eq( Volt::Frontend::TypeKind::Int16 )
      Volt::Frontend::Type::INT32.kind.should eq( Volt::Frontend::TypeKind::Int32 )
      Volt::Frontend::Type::INT.kind.should eq( Volt::Frontend::TypeKind::Int )
      Volt::Frontend::Type::FLOAT.kind.should eq( Volt::Frontend::TypeKind::Float )
      Volt::Frontend::Type::FLOAT32.kind.should eq( Volt::Frontend::TypeKind::Float32 )
      Volt::Frontend::Type::FLOAT64.kind.should eq( Volt::Frontend::TypeKind::Float64 )
      Volt::Frontend::Type::BOOL.kind.should eq( Volt::Frontend::TypeKind::Bool )
      Volt::Frontend::Type::STR.kind.should eq( Volt::Frontend::TypeKind::Str )
      Volt::Frontend::Type::REGEX.kind.should eq( Volt::Frontend::TypeKind::Regex )
      Volt::Frontend::Type::NIL.kind.should eq( Volt::Frontend::TypeKind::Nil )
      Volt::Frontend::Type::UNKNOWN.kind.should eq( Volt::Frontend::TypeKind::Unknown )
    end

    it "checks numeric types" do
      int_params = [] of Volt::Frontend::Type
      float_params = [] of Volt::Frontend::Type
      str_params = [] of Volt::Frontend::Type
      int_type = Volt::Frontend::Type.new( Volt::Frontend::TypeKind::Int, int_params, nil )
      float_type = Volt::Frontend::Type.new( Volt::Frontend::TypeKind::Float, float_params, nil )
      str_type = Volt::Frontend::Type.new( Volt::Frontend::TypeKind::Str, str_params, nil )
      int_type.numeric?.should be_true
      float_type.numeric?.should be_true
      str_type.numeric?.should be_false
    end

    it "checks integer types" do
      int8_params = [] of Volt::Frontend::Type
      int32_params = [] of Volt::Frontend::Type
      float_params = [] of Volt::Frontend::Type
      int8_type = Volt::Frontend::Type.new( Volt::Frontend::TypeKind::Int8, int8_params, nil )
      int32_type = Volt::Frontend::Type.new( Volt::Frontend::TypeKind::Int32, int32_params, nil )
      float_type = Volt::Frontend::Type.new( Volt::Frontend::TypeKind::Float, float_params, nil )
      int8_type.integer?.should be_true
      int32_type.integer?.should be_true
      float_type.integer?.should be_false
    end

    it "checks float types" do
      float32_params = [] of Volt::Frontend::Type
      float64_params = [] of Volt::Frontend::Type
      int_params = [] of Volt::Frontend::Type
      float32_type = Volt::Frontend::Type.new( Volt::Frontend::TypeKind::Float32, float32_params, nil )
      float64_type = Volt::Frontend::Type.new( Volt::Frontend::TypeKind::Float64, float64_params, nil )
      int_type = Volt::Frontend::Type.new( Volt::Frontend::TypeKind::Int, int_params, nil )
      float32_type.float?.should be_true
      float64_type.float?.should be_true
      int_type.float?.should be_false
    end

    it "checks nominal types" do
      object_params = [] of Volt::Frontend::Type
      struct_params = [] of Volt::Frontend::Type
      int_params = [] of Volt::Frontend::Type
      object_type = Volt::Frontend::Type.new( Volt::Frontend::TypeKind::Object, object_params, nil )
      struct_type = Volt::Frontend::Type.new( Volt::Frontend::TypeKind::Struct, struct_params, nil )
      int_type = Volt::Frontend::Type.new( Volt::Frontend::TypeKind::Int, int_params, nil )
      object_type.nominal?.should be_true
      struct_type.nominal?.should be_true
      int_type.nominal?.should be_false
    end

    it "checks reference types" do
      object_params = [] of Volt::Frontend::Type
      str_params = [] of Volt::Frontend::Type
      int_params = [] of Volt::Frontend::Type
      func_params = [] of Volt::Frontend::Type
      object_type = Volt::Frontend::Type.new( Volt::Frontend::TypeKind::Object, object_params, nil )
      str_type = Volt::Frontend::Type.new( Volt::Frontend::TypeKind::Str, str_params, nil )
      func_type = Volt::Frontend::Type.func( func_params, Volt::Frontend::Type::NIL )
      int_type = Volt::Frontend::Type.new( Volt::Frontend::TypeKind::Int, int_params, nil )
      object_type.reference?.should be_true
      str_type.reference?.should be_true
      func_type.reference?.should be_true
      int_type.reference?.should be_false
    end

  end


end