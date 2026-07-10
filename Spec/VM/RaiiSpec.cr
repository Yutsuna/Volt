require "spec"
require "../../Source/Volt/__all__"


module Volt::Spec


  describe "VM::RAII" do


    it "INIT allocates a HeapObject" do
      chunk = Volt::IR::Chunk.new( "test_init", 0 )
      chunk.num_registers = 1
      chunk.code << Volt::IR::Instruction.abx( Volt::IR::Opcode::INIT, 0, 42 )
      chunk.code << Volt::IR::Instruction.abc( Volt::IR::Opcode::RET, 0, 0, 0 )

      unit = Volt::Compiler::Unit.new( [ chunk ], 0 )
      vm   = Volt::VM::Vm.new( unit )

      result = vm.call_chunk( chunk, [] of Volt::IR::Value ).first
      result.as_object.type_id.should eq( 42 )
      result.as_object.destroyed?.should be_false
    end


    it "DROP destroys the object and nils the register (smoke test)" do
      chunk = Volt::IR::Chunk.new( "test_drop", 0 )
      chunk.num_registers = 2
      chunk.code << Volt::IR::Instruction.abx( Volt::IR::Opcode::INIT, 0, 42 )
      chunk.code << Volt::IR::Instruction.abx( Volt::IR::Opcode::DROP, 0, 42 )
      chunk.code << Volt::IR::Instruction.abc( Volt::IR::Opcode::LOAD_NIL, 1, 0, 0 )
      chunk.code << Volt::IR::Instruction.abc( Volt::IR::Opcode::RET, 1, 0, 0 )

      unit = Volt::Compiler::Unit.new( [ chunk ], 0 )
      vm   = Volt::VM::Vm.new( unit )

      result = vm.call_chunk( chunk, [] of Volt::IR::Value ).first
      result.is_nil?.should be_true
    end


    it "DROP_SCOPE destroys multiple objects" do
      chunk = Volt::IR::Chunk.new( "test_drop_scope", 0 )
      chunk.num_registers = 3

      chunk.scope_tables << [ 0, 1 ]

      chunk.code << Volt::IR::Instruction.abx( Volt::IR::Opcode::INIT, 0, 10 )
      chunk.code << Volt::IR::Instruction.abx( Volt::IR::Opcode::INIT, 1, 20 )
      chunk.code << Volt::IR::Instruction.abx( Volt::IR::Opcode::DROP_SCOPE, 0, 0 )
      chunk.code << Volt::IR::Instruction.abc( Volt::IR::Opcode::LOAD_NIL, 2, 0, 0 )
      chunk.code << Volt::IR::Instruction.abc( Volt::IR::Opcode::RET, 2, 0, 0 )

      unit = Volt::Compiler::Unit.new( [ chunk ], 0 )
      vm   = Volt::VM::Vm.new( unit )

      result = vm.call_chunk( chunk, [] of Volt::IR::Value ).first
      result.is_nil?.should be_true
    end


    it "Unwind destroys live objects on exception" do
      chunk = Volt::IR::Chunk.new( "test_unwind", 0 )
      chunk.num_registers = 2

      chunk.constants << Volt::IR::Value.int( 42_i64 )

      chunk.code << Volt::IR::Instruction.abx( Volt::IR::Opcode::INIT, 0, 99 )
      chunk.code << Volt::IR::Instruction.abx( Volt::IR::Opcode::LOAD_CONST, 1, 0 )
      chunk.code << Volt::IR::Instruction.abc( Volt::IR::Opcode::RAISE, 1, 0, 0 )

      chunk.drop_map.entries << Volt::IR::DropEntry.new(
        pc_start: 0_u32,
        pc_end:   UInt32::MAX,
        register: 0_u8,
        type_id:  99
      )

      unit = Volt::Compiler::Unit.new( [ chunk ], 0 )
      vm   = Volt::VM::Vm.new( unit )

      exit_code = vm.run
      exit_code.should eq( 1 )
    end


  end


end
