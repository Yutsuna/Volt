require "spec"
require "../../../Source/Volt/__all__"


module Volt::Spec


  describe "Volt::Frontend::SimpleType" do

    it "initializes with name and location" do
      span = Volt::Frontend::Span.new( "test.vl", 1, 1, 0, 0 )
      type = Volt::Frontend::SimpleType.new( "Int64", span )
      type.name.should eq( "Int64" )
      type.loc.should eq( span )
    end

  end


  describe "Volt::Frontend::GenericType" do

    it "initializes with name, params, and location" do
      span = Volt::Frontend::Span.new( "test.vl", 1, 1, 0, 0 )
      param_span = Volt::Frontend::Span.new( "test.vl", 1, 1, 0, 0 )
      params : Array( Volt::Frontend::ATypeNode ) = [] of Volt::Frontend::ATypeNode
      string_type = Volt::Frontend::SimpleType.new( "String", param_span )
      params << string_type
      type = Volt::Frontend::GenericType.new( "Array", params, span )
      type.name.should eq( "Array" )
      type.params.should eq( params )
      type.loc.should eq( span )
    end

    it "initializes with multiple type parameters" do
      span = Volt::Frontend::Span.new( "test.vl", 1, 1, 0, 0 )
      param_span = Volt::Frontend::Span.new( "test.vl", 1, 1, 0, 0 )
      params = [] of Volt::Frontend::ATypeNode
      params << Volt::Frontend::SimpleType.new( "K", param_span )
      params << Volt::Frontend::SimpleType.new( "V", param_span )
      type = Volt::Frontend::GenericType.new( "Hash", params, span )
      type.name.should eq( "Hash" )
      type.params.should eq( params )
    end

  end


  describe "Volt::Frontend::FuncType" do

    it "initializes with params, return_type, and location" do
      span = Volt::Frontend::Span.new( "test.vl", 1, 1, 0, 0 )
      param_span = Volt::Frontend::Span.new( "test.vl", 1, 1, 0, 0 )
      return_span = Volt::Frontend::Span.new( "test.vl", 1, 1, 0, 0 )
      params = [] of Volt::Frontend::ATypeNode
params << Volt::Frontend::SimpleType.new( "Int64", param_span )
      return_type = Volt::Frontend::SimpleType.new( "String", return_span )
      type = Volt::Frontend::FuncType.new( params, return_type, span )
      type.params.should eq( params )
      type.return_type.should eq( return_type )
      type.loc.should eq( span )
    end

    it "initializes with no parameters" do
      span = Volt::Frontend::Span.new( "test.vl", 1, 1, 0, 0 )
      return_span = Volt::Frontend::Span.new( "test.vl", 1, 1, 0, 0 )
      return_type = Volt::Frontend::SimpleType.new( "Void", return_span )
      params = [] of Volt::Frontend::ATypeNode
      type = Volt::Frontend::FuncType.new( params, return_type, span )
      type.params.should be_empty
      type.return_type.should eq( return_type )
    end

  end


  describe "Volt::Frontend::NilableType" do

    it "initializes with inner type and location" do
      span = Volt::Frontend::Span.new( "test.vl", 1, 1, 0, 0 )
      inner_span = Volt::Frontend::Span.new( "test.vl", 1, 1, 0, 0 )
      inner = Volt::Frontend::SimpleType.new( "String", inner_span )
      type = Volt::Frontend::NilableType.new( inner, span )
      type.inner.should eq( inner )
      type.loc.should eq( span )
    end

  end


end