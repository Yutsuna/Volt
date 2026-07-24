// Bytecode.inl — the single manifest of VM opcodes.
//
// Re-included with different definitions of VOLT_OP to generate the opcode
// enum, the name LUT and the operand-width table (meta-first: adding an op
// is one line here, and every consumer — disassembler, interpreter switch,
// emitter — picks it up without another edit).
//
//        Op              OperandBytes  (immediate bytes following the opcode)
#ifndef VOLT_OP
    #define VOLT_OP( Name, OperandBytes )
#endif

VOLT_OP( Nop, 0 )
// Constants & stack shape
VOLT_OP( LoadConst, 2 ) // u16 constant-pool index
VOLT_OP( Pop, 0 )
VOLT_OP( Dup, 0 )
// Locals / captures / fields
VOLT_OP( LoadLocal, 2 ) // u16 frame slot
VOLT_OP( StoreLocal, 2 )
VOLT_OP( LoadUpvalue, 2 ) // u16 closure-env field index
VOLT_OP( StoreUpvalue, 2 )
VOLT_OP( LoadField, 2 ) // u16 byte offset (LayoutEngine::FieldOffset)
VOLT_OP( StoreField, 2 )
// Calls — CallPrim covers what CalleeResolution left unresolved: a machine
// operation selected from Primitive{ Spelling, Bits }.
VOLT_OP( Call, 3 )         // u16 function-table index + u8 argc
VOLT_OP( CallIndirect, 1 ) // u8 argc, callee on stack (closures)
VOLT_OP( CallPrim, 2 )     // u16 primitive-op table index
VOLT_OP( MakeClosure, 3 )  // u16 function-table index + u8 capture count
VOLT_OP( Return, 0 )
// Control flow
VOLT_OP( Jump, 2 ) // i16 relative
VOLT_OP( JumpIfFalse, 2 )
// Exceptions
VOLT_OP( Raise, 0 )
VOLT_OP( EnterRescue, 2 ) // i16 handler offset
VOLT_OP( LeaveRescue, 0 )
VOLT_OP( Halt, 0 )

#undef VOLT_OP
