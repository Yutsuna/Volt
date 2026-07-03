# Spec/REPL/REPLDeltaCompilerSpec.cr
require "spec"
require "../../Source/Volt/__all__"

describe "REPL::REPLDeltaCompiler & VM::Vm extension" do

  it "compiles and runs incremental chunks, resolving top-level variables and remapping indices" do
    # 1. First program: x = 42
    p1 = Volt::Frontend.parse( "x = 42", "<repl>" )
    state = Volt::REPL::IncrementalState.new
    analyser1 = Volt::Frontend::IncrementalAnalyser.new( p1, state )
    typed1 = analyser1.analyse
    
    # Compile the first chunk using REPLDeltaCompiler
    compiler1 = Volt::REPL::REPLDeltaCompiler.new(
      typed1,
      {} of String => Int32,
      {} of String => Int32,
      [] of Volt::Compiler::NativeFunc,
      0,
      state.top_level_globals,
      0
    )
    unit1 = compiler1.compile
    
    # Initialize the VM with the first unit
    vm = Volt::VM::Vm.new( unit1, STDOUT, STDERR )
    # Execute the first main chunk
    vm.call_chunk_at( unit1.main_index )
    
    # 2. Second program: y = x + 10
    p2 = Volt::Frontend.parse( "y = x + 10", "<repl>" )
    analyser2 = Volt::Frontend::IncrementalAnalyser.new( p2, state )
    typed2 = analyser2.analyse
    
    # Create the delta compiler for the second program
    existing_chunk_count = unit1.chunks.size
    
    compiler = Volt::REPL::REPLDeltaCompiler.new(
      typed2,
      compiler1.func_index,
      compiler1.global_index,
      unit1.natives,
      existing_chunk_count,
      state.top_level_globals,
      compiler1.globals_count
    )
    
    delta_unit = compiler.compile
    
    # We should have compiled a delta unit
    delta_unit.chunks.should_not be_empty
    
    # Extend the VM with the delta unit
    vm.extend( delta_unit )
    
    # Execute the delta's main chunk
    absolute_main_idx = existing_chunk_count + delta_unit.main_index
    vm.call_chunk_at( absolute_main_idx )
    
    # 3. Third program: y
    p3 = Volt::Frontend.parse( "y", "<repl>" )
    analyser3 = Volt::Frontend::IncrementalAnalyser.new( p3, state )
    typed3 = analyser3.analyse
    
    compiler3 = Volt::REPL::REPLDeltaCompiler.new(
      typed3,
      compiler.func_index,
      compiler.global_index,
      delta_unit.natives,
      existing_chunk_count + delta_unit.chunks.size,
      state.top_level_globals,
      compiler.globals_count
    )
    delta_unit3 = compiler3.compile
    vm.extend( delta_unit3 )
    
    res = vm.call_chunk_at( existing_chunk_count + delta_unit.chunks.size + delta_unit3.main_index )
    res.first.as_i.should eq( 52 )
  end

end
