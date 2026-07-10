require "spec"
require "../../Source/Volt/__all__"


module Volt::Spec


  describe "VM::Opcode ABI (Source/C/Dispatch.c)" do

    it "keeps opcode ordinals stable for the C DispatchTable" do
      Volt::IR::Opcode::LOAD_CONST.value.should    eq 0
      Volt::IR::Opcode::MOVE.value.should          eq 4
      Volt::IR::Opcode::ADD_INT.value.should       eq 5
      Volt::IR::Opcode::NEG_F64.value.should       eq 15
      Volt::IR::Opcode::EQ.value.should            eq 24
      Volt::IR::Opcode::EQ_INT.value.should        eq 26
      Volt::IR::Opcode::NOT.value.should           eq 28
      Volt::IR::Opcode::JMP.value.should           eq 29
      Volt::IR::Opcode::CALL.value.should          eq 31
      Volt::IR::Opcode::CALL_NATIVE.value.should   eq 32
      Volt::IR::Opcode::RET.value.should           eq 33
      Volt::IR::Opcode::CONV_INT.value.should      eq 42
       Volt::IR::Opcode::CALL_METHOD.value.should   eq 51
      Volt::IR::Opcode::LOAD_GLOBAL.value.should   eq 55
      Volt::IR::Opcode::STORE_GLOBAL.value.should  eq 56
      Volt::IR::Opcode::NOP.value.should           eq 57
      Volt::IR::Opcode::ADD_INT_IMM.value.should   eq 58
      Volt::IR::Opcode::BR_LT_INT.value.should     eq 62
      Volt::IR::Opcode::BR_NE_INT_IMM.value.should eq 73
      Volt::IR::Opcode::LOAD_PTR.value.should      eq 74
      Volt::IR::Opcode::STORE_PTR.value.should     eq 75
      Volt::IR::Opcode::PTR_ADD.value.should       eq 76
      Volt::IR::Opcode::PTR_SUB.value.should       eq 77
      Volt::IR::Opcode::ADDR_LOCAL.value.should    eq 78
      Volt::IR::Opcode::ADDR_FIELD.value.should    eq 79
    end

    it "keeps the opcode count at what the DispatchTable covers (0..79)" do
      # Bump this and the C DispatchTable together when adding opcodes.
      Volt::IR::Opcode.values.size.should eq 80
    end

    it "keeps Value::Tag ordinals stable for the C EValueTag" do
      Volt::IR::Value::Tag::Int.value.should    eq 0
      Volt::IR::Value::Tag::Float.value.should  eq 1
      Volt::IR::Value::Tag::Bool.value.should   eq 2
      Volt::IR::Value::Tag::Regex.value.should  eq 3
      Volt::IR::Value::Tag::Nil.value.should    eq 4
      Volt::IR::Value::Tag::Object.value.should eq 5
      Volt::IR::Value::Tag::Ptr.value.should    eq 6
    end

  end


end
