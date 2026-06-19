describe "Phase 1 examples" do

  it "01.00.vl reports the correct type for every literal form" do
    code, out = SpecHelper.run_example("01.00.vl")
    code.should eq(0)
    out.should eq([
      "Int32", "Float64", "Float32", "Float64", "Bool", "Bool",
      "Char", "UInt8*", "UInt8*", "Int32*",
      "Int32", "Int32", "Int32",
      "UInt8", "UInt16", "UInt32", "UInt64",
      "Int8", "Int16", "Int64",
      "Int32", "Int32*",
      "Float64", "Float64", "Nil",
    ].join("\n") + "\n")
  end

  it "01.a.Return returns the assigned exit code" do
    code, _ = SpecHelper.run_example("01.a.Return.vl")
    code.should eq(84)
  end

  it "01.b.Arithmetic computes 270 (exit code 270 & 0xFF = 14)" do
    code, _ = SpecHelper.run_example("01.b.Arithmetic.vl")
    code.should eq(14)
  end

  it "01.c.HelloWorld prints the greeting" do
    code, out = SpecHelper.run_example("01.c.HelloWorld.vl")
    code.should eq(0)
    out.should eq("Hello, World!\n")
  end

  it "01.d.Conditions selects the 'Excellent' branch" do
    code, out = SpecHelper.run_example("01.d.Conditions.vl")
    code.should eq(0)
    out.should eq("Excellent\n")
  end

  it "01.e.Loops counts up to 200" do
    code, _ = SpecHelper.run_example("01.e.Loops.vl")
    code.should eq(0)
  end

  it "01.f.Arrays iterates correctly" do
    code, _ = SpecHelper.run_example("01.f.Arrays.vl")
    code.should eq(0)
  end

  it "01.g.TypePromotion promotes and re-types" do
    code, out = SpecHelper.run_example("01.g.TypePromotion.vl")
    code.should eq(0)
    out.should eq("Float64\nInt32\n")
  end

  it "01.h.Comparaison evaluates all operators correctly" do
    code, _ = SpecHelper.run_example("01.h.Comparaison.vl")
    code.should eq(0)
  end

end
