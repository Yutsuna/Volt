module Volt::VM


  class Vm

    private def exec_pointer( frame : Frame, chunk : IR::Chunk, ins : IR::Instruction )
      case ins.op
      when .load_ptr?
        ptr = frame[ ins.b ].as_ptr
        if ptr.null?
          raise VoltRuntimeError.new( "null pointer exception" )
        end
        val = case ins.c
              when 0 # WIDTH_I8
                IR::Value.int( ptr.as( Int8* ).value.to_i64 )
              when 1 # WIDTH_I16
                IR::Value.int( ptr.as( Int16* ).value.to_i64 )
              when 2 # WIDTH_I32
                IR::Value.int( ptr.as( Int32* ).value.to_i64 )
              when 3 # WIDTH_I64
                IR::Value.int( ptr.as( Int64* ).value )
              when 4 # WIDTH_U8
                IR::Value.int( ptr.as( UInt8* ).value.to_i64 )
              when 5 # WIDTH_U16
                IR::Value.int( ptr.as( UInt16* ).value.to_i64 )
              when 6 # WIDTH_U32
                IR::Value.int( ptr.as( UInt32* ).value.to_i64 )
              when 7 # WIDTH_U64
                IR::Value.int( ptr.as( UInt64* ).value.to_i64 )
              when 8 # WIDTH_PTR
                IR::Value.ptr( ptr.as( Pointer( Void )* ).value )
              else
                raise VoltRuntimeError.new( "unknown pointer width code #{ins.c}" )
              end
        frame[ ins.a ] = val

      when .store_ptr?
        ptr = frame[ ins.a ].as_ptr
        if ptr.null?
          raise VoltRuntimeError.new( "null pointer exception" )
        end
        val = frame[ ins.b ]
        case ins.c
        when 0, 4 # WIDTH_I8 / WIDTH_U8
          ptr.as( UInt8* ).value = val.as_i.to_u8
        when 1, 5 # WIDTH_I16 / WIDTH_U16
          ptr.as( UInt16* ).value = val.as_i.to_u16
        when 2, 6 # WIDTH_I32 / WIDTH_U32
          ptr.as( UInt32* ).value = val.as_i.to_u32
        when 3, 7 # WIDTH_I64 / WIDTH_U64
          ptr.as( UInt64* ).value = val.as_i.to_u64
        when 8 # WIDTH_PTR
          ptr.as( Pointer( Void )* ).value = val.as_ptr
        else
          raise VoltRuntimeError.new( "unknown pointer width code #{ins.c}" )
        end

      when .ptr_add?
        ptr = frame[ ins.b ].as_ptr
        offset = frame[ ins.c ].as_i
        frame[ ins.a ] = IR::Value.ptr( ptr + offset )

      when .ptr_sub?
        ptr = frame[ ins.b ].as_ptr
        offset = frame[ ins.c ].as_i
        frame[ ins.a ] = IR::Value.ptr( ptr - offset )

      when .addr_local?
        payload_ptr = ( ( frame.regs_ptr + ins.b ).as( UInt8* ) + 8 ).as( Pointer( Void ) )
        frame[ ins.a ] = IR::Value.ptr( payload_ptr )

      when .addr_field?
        obj = frame[ ins.b ].as_object
        if obj.to_unsafe.null?
          raise VoltRuntimeError.new( "nil receiver" )
        end
        payload_ptr = ( ( obj.fields + ins.c ).as( UInt8* ) + 8 ).as( Pointer( Void ) )
        frame[ ins.a ] = IR::Value.ptr( payload_ptr )

      else
        raise VoltRuntimeError.new( "unhandled pointer opcode #{ins.op}" )
      end
    end

  end


end
