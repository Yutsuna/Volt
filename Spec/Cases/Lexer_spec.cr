describe Volt::Lexer::FLexer do

  it "decodes integer literals across bases and suffixes" do
    tokens = SpecHelper.lex("0x1A 0b1010 1_000_000 1u8")
    ints = tokens.select { |t| t.kind == Volt::Lexer::EToken::Integer }
    ints.map(&.int_value).should eq([26_i64, 10_i64, 1_000_000_i64, 1_i64])
    ints.last.suffix.should eq("u8")
  end

  it "normalises float suffix case" do
    tokens = SpecHelper.lex("1.0F64")
    float = tokens.first
    float.kind.should eq(Volt::Lexer::EToken::Float)
    float.suffix.should eq("f64")
  end

  it "suppresses the newline after a binary operator so the expression continues" do
    kinds = SpecHelper.lex("a +\nb").map(&.kind)
    kinds[0, 3].should eq([
      Volt::Lexer::EToken::Identifier,
      Volt::Lexer::EToken::Plus,
      Volt::Lexer::EToken::Identifier,
    ])
  end

end


describe Volt::Types::Type do

  it "renders the typeof names the master test expects" do
    Volt::Types::Type.new(Volt::Types::EType::Int32).to_s.should eq("Int32")
    Volt::Types::Type.new(Volt::Types::EType::UInt8, 1).to_s.should eq("UInt8*")
    Volt::Types::Type.new(Volt::Types::EType::Int32, 1).to_s.should eq("Int32*")
    Volt::Types::Type.new(Volt::Types::EType::Nil).to_s.should eq("Nil")
    Volt::Types::Type.new(Volt::Types::EType::Void).to_s.should eq("Void")
  end

end
