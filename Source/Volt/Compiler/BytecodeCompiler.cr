module Volt::Compiler


  class BytecodeCompiler
    @func_index : Hash( String, Int32 )

    #------------------------------------------------------------------------------------

    def initialize( @typed : Frontend::TypedProgram )
      @func_index = {} of String => Int32
      @natives    = NativeTable.new
    end

    #------------------------------------------------------------------------------------

    def compile : Unit
      @typed.functions.each_with_index { |fn, i| @func_index[ fn.name ] = i }
      chunks = @typed.functions.map { |fn| compile_function( fn ) }
      chunks << compile_main
      Unit.new( chunks, chunks.size - 1, @natives.names )
    end

    #------------------------------------------------------------------------------------

    private def compile_function( fn : Frontend::FuncDecl ) : IR::Chunk
      emitter = FnEmitter.new( fn.name, fn.params.size, @func_index, @typed.signatures, @natives )
      fn.params.each { |p| emitter.bind_param( p.name ) }
      result = emitter.compile_body( fn.body )
      emitter.emit_ret( result )
      emitter.finish
    end

    private def compile_main : IR::Chunk
      emitter = FnEmitter.new( "main", 0, @func_index, @typed.signatures, @natives )
      result = emitter.compile_body( @typed.top_level )
      emitter.emit_ret( result )
      emitter.finish
    end

    #------------------------------------------------------------------------------------

  end


  class NativeTable
    getter names : Array( String )

    #------------------------------------------------------------------------------------

    def initialize
      @names = [] of String
      @index = {} of String => Int32
    end

    #------------------------------------------------------------------------------------

    def intern( name : String ) : Int32
      @index[ name ] ||= begin
        @names << name
        @names.size - 1
      end
    end

    #------------------------------------------------------------------------------------

  end


  class FnEmitter

    #------------------------------------------------------------------------------------

    def initialize( name : String, arity : Int32,
                    @func_index : Hash( String, Int32 ),
                    @signatures : Hash( String, Frontend::FuncSig ),
                    @natives : NativeTable )
      @chunk    = IR::Chunk.new( name, arity )
      @scope    = {} of String => Int32
      @next_reg = 0
      @max_reg  = 0
    end

    #------------------------------------------------------------------------------------

    def bind_param( name : String )
      @scope[ name ] = alloc
    end

    def finish : IR::Chunk
      @chunk.num_registers = @max_reg
      @chunk
    end

    #------------------------------------------------------------------------------------

    private def alloc : Int32
      r          = @next_reg
      @next_reg += 1
      @max_reg   = @next_reg if @next_reg > @max_reg
      r
    end

    private def alloc_block( n : Int32 ) : Int32
      base       = @next_reg
      @next_reg += n
      @max_reg   = @next_reg if @next_reg > @max_reg
      base
    end

    #------------------------------------------------------------------------------------

    private def emit_abc( op : IR::Opcode, a : Int32, b : Int32, c : Int32 )
      @chunk.code << IR::Instruction.abc( op, a, b, c )
    end

    private def emit_abx( op : IR::Opcode, a : Int32, bx : Int32 )
      @chunk.code << IR::Instruction.abx( op, a, bx )
    end

    private def here : Int32
      @chunk.code.size
    end

    # Emit a placeholder jump; returns its index for later patching.
    private def emit_jump_placeholder( op : IR::Opcode, a : Int32 ) : Int32
      idx = here
      emit_abx( op, a, 0 )
      idx
    end

    private def patch_jump( at : Int32, target : Int32 )
      old = @chunk.code[ at ]
      @chunk.code[ at ] = IR::Instruction.abx( old.op, old.a, target )
    end

    private def add_const( v : IR::Value ) : Int32
      @chunk.constants << v
      @chunk.constants.size - 1
    end

    def emit_ret( reg : Int32 )
      emit_abc( IR::Opcode::RET, reg, 0, 0 )
    end

    #------------------------------------------------------------------------------------

    # Compiles a statement sequence; returns the register holding the last value.
    def compile_body( nodes : Array( Frontend::ANode ) ) : Int32
      last = -1
      nodes.each do |node|
        last = compile_expr( node.as( Frontend::AExpr ) )
      end
      if last < 0
        last = alloc
        emit_abc( IR::Opcode::LOAD_NIL, last, 0, 0 )
      end
      last
    end

    #------------------------------------------------------------------------------------

    def compile_expr( expr : Frontend::AExpr ) : Int32
      case expr
      when Frontend::IntLit     then const_reg( IR::Value.int( expr.value ) )
      when Frontend::FloatLit   then const_reg( IR::Value.float( expr.value ) )
      when Frontend::StringLit  then const_reg( IR::Value.str( expr.value ) )
      when Frontend::BoolLit
        r = alloc
        emit_abc( expr.value ? IR::Opcode::LOAD_TRUE : IR::Opcode::LOAD_FALSE, r, 0, 0 )
        r
      when Frontend::NilLit
        r = alloc
        emit_abc( IR::Opcode::LOAD_NIL, r, 0, 0 )
        r
      when Frontend::Ident      then @scope[ expr.name ]
      when Frontend::Assign     then compile_assign( expr )
      when Frontend::BinaryOp   then compile_binary( expr )
      when Frontend::UnaryOp    then compile_unary( expr )
      when Frontend::Call       then compile_call( expr )
      when Frontend::IfExpr     then compile_if( expr )
      when Frontend::WhileExpr  then compile_while( expr )
      when Frontend::ReturnExpr then compile_return( expr )
      else
        raise "internal: unlowerable node #{expr.class.name} (should be rejected by Semantic)"
      end
    end

    private def const_reg( v : IR::Value ) : Int32
      r   = alloc
      idx = add_const( v )
      emit_abx( IR::Opcode::LOAD_CONST, r, idx )
      r
    end

    private def compile_assign( expr : Frontend::Assign ) : Int32
      name      = expr.target.as( Frontend::Ident ).name
      value_reg = compile_expr( expr.value )
      if home = @scope[ name ]?
        emit_abc( IR::Opcode::MOVE, home, value_reg, 0 )
        home
      else
        @scope[ name ] = value_reg
        value_reg
      end
    end

    private def compile_binary( expr : Frontend::BinaryOp ) : Int32
      case expr.op
      when .and? then return compile_and( expr )
      when .or?  then return compile_or( expr )
      end

      lreg   = compile_expr( expr.left )
      rreg   = compile_expr( expr.right )
      is_f64 = numeric_float?( expr.left )
      dest   = alloc
      emit_abc( binary_opcode( expr.op, is_f64 ), dest, lreg, rreg )
      dest
    end

    private def compile_and( expr : Frontend::BinaryOp ) : Int32
      dest = alloc
      lreg = compile_expr( expr.left )
      emit_abc( IR::Opcode::MOVE, dest, lreg, 0 )
      skip = emit_jump_placeholder( IR::Opcode::JMP_IF_FALSE, dest )
      rreg = compile_expr( expr.right )
      emit_abc( IR::Opcode::MOVE, dest, rreg, 0 )
      patch_jump( skip, here )
      dest
    end

    private def compile_or( expr : Frontend::BinaryOp ) : Int32
      dest = alloc
      lreg = compile_expr( expr.left )
      emit_abc( IR::Opcode::MOVE, dest, lreg, 0 )
      neg  = alloc
      emit_abc( IR::Opcode::NOT, neg, dest, 0 )
      skip = emit_jump_placeholder( IR::Opcode::JMP_IF_FALSE, neg )
      rreg = compile_expr( expr.right )
      emit_abc( IR::Opcode::MOVE, dest, rreg, 0 )
      patch_jump( skip, here )
      dest
    end

    private def compile_unary( expr : Frontend::UnaryOp ) : Int32
      oreg = compile_expr( expr.operand )
      dest = alloc
      case expr.op
      when .minus?
        op = numeric_float?( expr.operand ) ? IR::Opcode::NEG_F64 : IR::Opcode::NEG_INT
        emit_abc( op, dest, oreg, 0 )
      else # bang / not
        emit_abc( IR::Opcode::NOT, dest, oreg, 0 )
      end
      dest
    end

    private def compile_call( expr : Frontend::Call ) : Int32
      name     = expr.callee.as( Frontend::Ident ).name
      sig      = @signatures[ name ]
      arg_regs = expr.args.map { |a| compile_expr( a ) }
      argc     = arg_regs.size
      base     = alloc_block( argc + 1 )
      arg_regs.each_with_index { |ar, i| emit_abc( IR::Opcode::MOVE, base + 1 + i, ar, 0 ) }
      if sig.extern
        emit_abc( IR::Opcode::CALL_NATIVE, base, @natives.intern( name ), argc )
      else
        emit_abc( IR::Opcode::CALL, base, @func_index[ name ], argc )
      end
      base
    end

    private def compile_if( expr : Frontend::IfExpr ) : Int32
      end_jumps = [] of Int32

      creg    = compile_expr( expr.cond )
      to_next = emit_jump_placeholder( IR::Opcode::JMP_IF_FALSE, creg )
      compile_body( expr.then_b )
      end_jumps << emit_jump_placeholder( IR::Opcode::JMP, 0 )
      patch_jump( to_next, here )

      expr.elsifs.each do |cond, body|
        creg    = compile_expr( cond )
        to_next = emit_jump_placeholder( IR::Opcode::JMP_IF_FALSE, creg )
        compile_body( body )
        end_jumps << emit_jump_placeholder( IR::Opcode::JMP, 0 )
        patch_jump( to_next, here )
      end

      if eb = expr.else_b
        compile_body( eb )
      end

      end_jumps.each { |j| patch_jump( j, here ) }

      dest = alloc
      emit_abc( IR::Opcode::LOAD_NIL, dest, 0, 0 )
      dest
    end

    private def compile_while( expr : Frontend::WhileExpr ) : Int32
      top    = here
      creg   = compile_expr( expr.cond )
      to_end = emit_jump_placeholder( IR::Opcode::JMP_IF_FALSE, creg )
      compile_body( expr.body )
      emit_abx( IR::Opcode::JMP, 0, top )
      patch_jump( to_end, here )

      dest = alloc
      emit_abc( IR::Opcode::LOAD_NIL, dest, 0, 0 )
      dest
    end

    private def compile_return( expr : Frontend::ReturnExpr ) : Int32
      reg = if v = expr.value
        compile_expr( v )
      else
        r = alloc
        emit_abc( IR::Opcode::LOAD_NIL, r, 0, 0 )
        r
      end
      emit_ret( reg )
      reg
    end

    #------------------------------------------------------------------------------------

    private def numeric_float?( expr : Frontend::AExpr ) : Bool
      ( t = expr.resolved_type ) ? t.kind.float? : false
    end

    private def binary_opcode( kind : Frontend::TokenKind, f64 : Bool ) : IR::Opcode
      case kind
      when .plus?        then f64 ? IR::Opcode::ADD_F64 : IR::Opcode::ADD_INT
      when .minus?       then f64 ? IR::Opcode::SUB_F64 : IR::Opcode::SUB_INT
      when .star?        then f64 ? IR::Opcode::MUL_F64 : IR::Opcode::MUL_INT
      when .slash?       then f64 ? IR::Opcode::DIV_F64 : IR::Opcode::DIV_INT
      when .percent?     then IR::Opcode::MOD_INT
      when .lt?          then f64 ? IR::Opcode::LT_F64 : IR::Opcode::LT_INT
      when .lt_eq?       then f64 ? IR::Opcode::LE_F64 : IR::Opcode::LE_INT
      when .gt?          then f64 ? IR::Opcode::GT_F64 : IR::Opcode::GT_INT
      when .gt_eq?       then f64 ? IR::Opcode::GE_F64 : IR::Opcode::GE_INT
      when .eq_eq?       then IR::Opcode::EQ
      when .bang_eq?     then IR::Opcode::NE
      else
        raise "internal: unhandled binary opcode #{kind}"
      end
    end
  end


end
