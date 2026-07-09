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

end
