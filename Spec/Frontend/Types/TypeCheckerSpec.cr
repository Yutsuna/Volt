require "spec"
require "../../../Source/Volt/__all__"

describe "Volt::Frontend::TypeChecker - Polymorphism & Reassignment" do

  it "permits reassigning a variable to different subclasses of its declared type" do
    source = <<-VOLT
      class Parent
        def initialize; end
      end
      class ChildA < Parent
        def initialize; end
      end
      class ChildB < Parent
        def initialize; end
      end

      obj : Parent = nil
      obj = ChildA.new()
      obj = ChildB.new()
    VOLT

    program = Volt::Frontend.parse(source, "test_spec.vl")
    Volt::Frontend.analyse(program)
  end

  it "maintains the wider declared type inside conditional branches" do
    source = <<-VOLT
      class Parent; def initialize; end; end
      class ChildA < Parent; def initialize; end; end
      class ChildB < Parent; def initialize; end; end

      obj : Parent = nil
      cond = true
      if cond
        obj = ChildA.new()
      else
        obj = ChildB.new()
      end
    VOLT

    program = Volt::Frontend.parse(source, "test_spec_branch.vl")
    Volt::Frontend.analyse(program)
  end

  it "typechecks typeof expressions" do
    source = <<-VOLT
      a = 42
      typeof a
      typeof( a )

      class B
        def initialize; end
      end
      typeof( B )

      def c( d : Int32 ) -> Void
      end
      typeof( c )
    VOLT

    program = Volt::Frontend.parse(source, "test_typeof.vl")
    typed = Volt::Frontend.analyse(program)

    t1 = typed.top_level[1].as(Volt::Frontend::TypeofExpr)
    t1.resolved_type.should eq(Volt::Frontend::Type::STR)
    t1.resolved_operand_type.should eq(Volt::Frontend::Type::INT64)

    t2 = typed.top_level[2].as(Volt::Frontend::TypeofExpr)
    t2.resolved_type.should eq(Volt::Frontend::Type::STR)
    t2.resolved_operand_type.should eq(Volt::Frontend::Type::INT64)

    t3 = typed.top_level[3].as(Volt::Frontend::TypeofExpr)
    t3.resolved_type.should eq(Volt::Frontend::Type::STR)
    t3.resolved_operand_type.try(&.to_s).should eq("B")

    t4 = typed.top_level[4].as(Volt::Frontend::TypeofExpr)
    t4.resolved_type.should eq(Volt::Frontend::Type::STR)
    t4.resolved_operand_type.try(&.to_s).should eq("(Int32) -> Nil")
  end

  it "typechecks typeof expressions with strict assertions, shadowing, pointer types, and newlines" do
    source = <<-VOLT
      class MyClass
        def initialize; end
      end

      # 1. Shadowing
      B = 10
      typeof B

      # 2. Parentheses & newlines
      a = 42
      typeof(
        a
      )

      # 3. Primitives
      typeof nil
      typeof true
      typeof 3.14
      typeof "hello"

      # 4. Pointers
      p : Int32* = nil
      typeof p

      # 5. Complex Expressions & Precedence
      typeof a + "!"
      typeof(a + 2)
    VOLT

    program = Volt::Frontend.parse(source, "test_typeof_extra.vl")
    typed = Volt::Frontend.analyse(program)

    t1 = typed.top_level[1].as(Volt::Frontend::TypeofExpr)
    t1.resolved_type.try(&.kind).should eq(Volt::Frontend::TypeKind::Str)
    t1.resolved_operand_type.try(&.kind).should eq(Volt::Frontend::TypeKind::Int)

    t3 = typed.top_level[3].as(Volt::Frontend::TypeofExpr)
    t3.resolved_operand_type.try(&.kind).should eq(Volt::Frontend::TypeKind::Int)

    t4 = typed.top_level[4].as(Volt::Frontend::TypeofExpr)
    t4.resolved_operand_type.try(&.kind).should eq(Volt::Frontend::TypeKind::Nil)

    t5 = typed.top_level[5].as(Volt::Frontend::TypeofExpr)
    t5.resolved_operand_type.try(&.kind).should eq(Volt::Frontend::TypeKind::Bool)

    t6 = typed.top_level[6].as(Volt::Frontend::TypeofExpr)
    t6.resolved_operand_type.try(&.kind).should eq(Volt::Frontend::TypeKind::Float)

    t7 = typed.top_level[7].as(Volt::Frontend::TypeofExpr)
    t7.resolved_operand_type.try(&.kind).should eq(Volt::Frontend::TypeKind::Str)

    t9 = typed.top_level[9].as(Volt::Frontend::TypeofExpr)
    t9.resolved_operand_type.try(&.pointer?).should be_true
    t9.resolved_operand_type.try(&.pointee.kind).should eq(Volt::Frontend::TypeKind::Int32)

    # Precedence check: typeof a + "!" parses as (typeof a) + "!"
    t10 = typed.top_level[10].as(Volt::Frontend::BinaryOp)
    t10.left.should be_a(Volt::Frontend::TypeofExpr)
    t10.resolved_type.try(&.kind).should eq(Volt::Frontend::TypeKind::Str)

    t11 = typed.top_level[11].as(Volt::Frontend::TypeofExpr)
    t11.resolved_operand_type.try(&.kind).should eq(Volt::Frontend::TypeKind::Int)
  end

  it "raises semantic errors for typeof with undefined variables" do
    source = "typeof undefined_var"
    program = Volt::Frontend.parse(source, "test_typeof_error.vl")
    expect_raises(Volt::Frontend::CompilationError) do
      Volt::Frontend.analyse(program)
    end
  end

end
