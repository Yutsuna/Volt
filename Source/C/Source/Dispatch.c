#include "Dispatch.h"

/* ---------------------------------------------------------------------------
 * FValue construction / access — bit-exact with IR::Value.
 * ------------------------------------------------------------------------- */

static inline FValue MakeInt( Int64 Value )
{
	FValue Result;
	Result.Tag     = VAL_INT;
	Result.Payload = (Void*)(IntPtr)Value;
	return Result;
}

static inline FValue MakeBool( int Value )
{
	FValue Result;
	Result.Tag     = VAL_BOOL;
	Result.Payload = (Void*)(IntPtr)( Value ? 1 : 0 );
	return Result;
}

static inline FValue MakeFloat( double Value )
{
	union { double D; Void* P; } Bits;
	FValue Result;
	Bits.D         = Value;
	Result.Tag     = VAL_FLOAT;
	Result.Payload = Bits.P;
	return Result;
}

static inline FValue MakeNil( Void )
{
	FValue Result;
	Result.Tag     = VAL_NIL;
	Result.Payload = (Void*)0;
	return Result;
}

static inline Int64 AsInt( FValue Value )
{
	return (Int64)(IntPtr)Value.Payload;
}

static inline double AsF64( FValue Value )
{
	union { Void* P; double D; } Bits;
	Bits.P = Value.Payload;
	return Bits.D;
}

/** Volt truthiness: only nil and false are falsy (mirror of Value#truthy?). */
static inline int IsTruthy( FValue Value )
{
	return !( Value.Tag == VAL_NIL || ( Value.Tag == VAL_BOOL && Value.Payload == (Void*)0 ) );
}

/** Floored integer division / modulo — matches Crystal's `//` and `%`. */
static inline Int64 FloorDiv( Int64 A, Int64 B )
{
	Int64 Q = A / B;
	Int64 R = A % B;
	if ( ( R != 0 ) && ( ( R < 0 ) != ( B < 0 ) ) )
	{
		Q -= 1;
	}
	return Q;
}

static inline Int64 FloorMod( Int64 A, Int64 B )
{
	Int64 R = A % B;
	if ( ( R != 0 ) && ( ( R < 0 ) != ( B < 0 ) ) )
	{
		R += B;
	}
	return R;
}

/* ---------------------------------------------------------------------------
 * Dispatch core.
 * ------------------------------------------------------------------------- */

Int32 Volt_Dispatch( FVmContext* Ctx )
{
	/* One computed-goto table shared by every entry; label addresses are fixed
	 * for the program's lifetime, so it is filled exactly once. Crystal fibers
	 * are cooperative (no preemptive data race on Inited). */
	static Void* DispatchTable[ 256 ];
	static int   Inited = 0;

	if ( !Inited )
	{
		for ( int I = 0; I < 256; ++I )
		{
			DispatchTable[ I ] = &&LBadOp;
		}
		DispatchTable[  0 ] = &&LLoadConst;
		DispatchTable[  1 ] = &&LLoadTrue;
		DispatchTable[  2 ] = &&LLoadFalse;
		DispatchTable[  3 ] = &&LLoadNil;
		DispatchTable[  4 ] = &&LMove;
		DispatchTable[  5 ] = &&LAddInt;
		DispatchTable[  6 ] = &&LSubInt;
		DispatchTable[  7 ] = &&LMulInt;
		DispatchTable[  8 ] = &&LDivInt;
		DispatchTable[  9 ] = &&LModInt;
		DispatchTable[ 10 ] = &&LNegInt;
		DispatchTable[ 11 ] = &&LAddF64;
		DispatchTable[ 12 ] = &&LSubF64;
		DispatchTable[ 13 ] = &&LMulF64;
		DispatchTable[ 14 ] = &&LDivF64;
		DispatchTable[ 15 ] = &&LNegF64;
		DispatchTable[ 16 ] = &&LLtInt;
		DispatchTable[ 17 ] = &&LLeInt;
		DispatchTable[ 18 ] = &&LGtInt;
		DispatchTable[ 19 ] = &&LGeInt;
		DispatchTable[ 20 ] = &&LLtF64;
		DispatchTable[ 21 ] = &&LLeF64;
		DispatchTable[ 22 ] = &&LGtF64;
		DispatchTable[ 23 ] = &&LGeF64;
		DispatchTable[ 24 ] = &&LCold;        /* EQ            */
		DispatchTable[ 25 ] = &&LCold;        /* NE            */
		DispatchTable[ 26 ] = &&LEqInt;
		DispatchTable[ 27 ] = &&LNeInt;
		DispatchTable[ 28 ] = &&LNot;
		DispatchTable[ 29 ] = &&LJmp;
		DispatchTable[ 30 ] = &&LJmpIfFalse;
		DispatchTable[ 31 ] = &&LCall;
		DispatchTable[ 32 ] = &&LCold;        /* CALL_NATIVE   */
		DispatchTable[ 33 ] = &&LRet;
		DispatchTable[ 34 ] = &&LAndInt;
		DispatchTable[ 35 ] = &&LOrInt;
		DispatchTable[ 36 ] = &&LXorInt;
		DispatchTable[ 37 ] = &&LCold;        /* SHL_INT       */
		DispatchTable[ 38 ] = &&LCold;        /* SHR_INT       */
		DispatchTable[ 39 ] = &&LNotInt;
		DispatchTable[ 40 ] = &&LIDivInt;
		DispatchTable[ 41 ] = &&LCold;        /* POW_INT       */
		DispatchTable[ 42 ] = &&LConvInt;
		DispatchTable[ 43 ] = &&LCold;        /* CMP_INT       */
		DispatchTable[ 44 ] = &&LCold;        /* MATCH_STR     */
		DispatchTable[ 45 ] = &&LCold;        /* NOT_MATCH_STR */
		DispatchTable[ 46 ] = &&LCold;        /* EQ_CASE       */
		DispatchTable[ 47 ] = &&LInitObj;     /* INIT          */
		DispatchTable[ 48 ] = &&LCold;        /* DROP          */
		DispatchTable[ 49 ] = &&LCold;        /* DROP_SCOPE    */
		DispatchTable[ 50 ] = &&LCold;        /* RAISE         */
		DispatchTable[ 51 ] = &&LInitObj;     /* INIT_OBJ      */
		DispatchTable[ 52 ] = &&LLoadField;   /* LOAD_FIELD    */
		DispatchTable[ 53 ] = &&LStoreField;  /* STORE_FIELD   */
		DispatchTable[ 54 ] = &&LCold;        /* CALL_METHOD   */
		DispatchTable[ 55 ] = &&LCold;        /* CALL_MIXIN    */
		DispatchTable[ 56 ] = &&LCopyBlock;   /* COPY_BLOCK    */
		DispatchTable[ 57 ] = &&LNewStruct;   /* NEW_STRUCT    */
		DispatchTable[ 58 ] = &&LLoadGlobal;
		DispatchTable[ 59 ] = &&LStoreGlobal;
		DispatchTable[ 60 ] = &&LCold;        /* TO_STRING     */
		DispatchTable[ 61 ] = &&LCold;        /* CONCAT_STR    */
		DispatchTable[ 62 ] = &&LNop;
		DispatchTable[ 63 ] = &&LAddIntImm;
		DispatchTable[ 64 ] = &&LSubIntImm;
		DispatchTable[ 65 ] = &&LEqIntImm;
		DispatchTable[ 66 ] = &&LNeIntImm;
		DispatchTable[ 67 ] = &&LBrLtInt;
		DispatchTable[ 68 ] = &&LBrLeInt;
		DispatchTable[ 69 ] = &&LBrGtInt;
		DispatchTable[ 70 ] = &&LBrGeInt;
		DispatchTable[ 71 ] = &&LBrEqInt;
		DispatchTable[ 72 ] = &&LBrNeInt;
		DispatchTable[ 73 ] = &&LBrLtIntImm;
		DispatchTable[ 74 ] = &&LBrLeIntImm;
		DispatchTable[ 75 ] = &&LBrGtIntImm;
		DispatchTable[ 76 ] = &&LBrGeIntImm;
		DispatchTable[ 77 ] = &&LBrEqIntImm;
		DispatchTable[ 78 ] = &&LBrNeIntImm;
		Inited = 1;
	}

	/* Hot locals. Rebound by REBIND on every CALL/RET/fall-through. */
	const FChunkInfo* Chunks   = Ctx->Chunks;
	FValue*           Stack    = Ctx->Stack;
	Int32           CurChunk = Ctx->CurChunk;
	Int32           Base     = Ctx->Base;
	Int32           Ip       = Ctx->Ip;
	Int32           FrameDepth = Ctx->FrameDepth;
	Int32           StackTop = Ctx->StackTop;

	const UInt32*   Code   = Chunks[ CurChunk ].Code;
	Int32           Size   = Chunks[ CurChunk ].CodeSize;
	const FValue*     Consts = Chunks[ CurChunk ].Consts;
	FValue*           Regs   = Stack + Base;

	UInt32 Ins = 0;
	UInt32 Op  = 0;

#define SAVE_CURSOR()             \
	Ctx->CurChunk   = CurChunk;   \
	Ctx->Base       = Base;       \
	Ctx->Ip         = Ip;         \
	Ctx->FrameDepth = FrameDepth; \
	Ctx->StackTop   = StackTop

#define REBIND()                          \
	Code   = Chunks[ CurChunk ].Code;     \
	Size   = Chunks[ CurChunk ].CodeSize; \
	Consts = Chunks[ CurChunk ].Consts;   \
	Regs   = Stack + Base

#define DISPATCH()                        \
	do {                                  \
		if ( Ip >= Size ) goto LFellOff;  \
		Ins = Code[ Ip++ ];               \
		Op  = Ins >> 24;                  \
		goto *DispatchTable[ Op ];        \
	} while ( 0 )

/* Operand accessors on the current instruction word. */
#define OP_A  ( (Int32)( ( Ins >> 16 ) & 0xFF ) )
#define OP_B  ( (Int32)( ( Ins >> 8 ) & 0xFF ) )
#define OP_C  ( (Int32)( Ins & 0xFF ) )
#define OP_BX ( (Int32)( Ins & 0xFFFF ) )

	DISPATCH();

	/* ---- load / move ---------------------------------------------------- */
LLoadConst:
	Regs[ OP_A ] = Consts[ OP_BX ];
	DISPATCH();
LLoadTrue:
	Regs[ OP_A ] = MakeBool( 1 );
	DISPATCH();
LLoadFalse:
	Regs[ OP_A ] = MakeBool( 0 );
	DISPATCH();
LLoadNil:
	Regs[ OP_A ] = MakeNil();
	DISPATCH();
LMove:
	Regs[ OP_A ] = Regs[ OP_B ];
	DISPATCH();

	/* ---- integer arithmetic (overflow-checked, like Crystal) ------------ */
LAddInt:
	{
		Int64 R;
		if ( __builtin_add_overflow( AsInt( Regs[ OP_B ] ), AsInt( Regs[ OP_C ] ), &R ) )
		{
			SAVE_CURSOR();
			return VM_ERR_OVERFLOW;
		}
		Regs[ OP_A ] = MakeInt( R );
	}
	DISPATCH();
LSubInt:
	{
		Int64 R;
		if ( __builtin_sub_overflow( AsInt( Regs[ OP_B ] ), AsInt( Regs[ OP_C ] ), &R ) )
		{
			SAVE_CURSOR();
			return VM_ERR_OVERFLOW;
		}
		Regs[ OP_A ] = MakeInt( R );
	}
	DISPATCH();
LMulInt:
	{
		Int64 R;
		if ( __builtin_mul_overflow( AsInt( Regs[ OP_B ] ), AsInt( Regs[ OP_C ] ), &R ) )
		{
			SAVE_CURSOR();
			return VM_ERR_OVERFLOW;
		}
		Regs[ OP_A ] = MakeInt( R );
	}
	DISPATCH();
LDivInt:
LIDivInt:
	{
		Int64 Divisor = AsInt( Regs[ OP_C ] );
		if ( Divisor == 0 )
		{
			SAVE_CURSOR();
			return VM_ERR_DIVZERO;
		}
		Regs[ OP_A ] = MakeInt( FloorDiv( AsInt( Regs[ OP_B ] ), Divisor ) );
	}
	DISPATCH();
LModInt:
	{
		Int64 Divisor = AsInt( Regs[ OP_C ] );
		if ( Divisor == 0 )
		{
			SAVE_CURSOR();
			return VM_ERR_DIVZERO;
		}
		Regs[ OP_A ] = MakeInt( FloorMod( AsInt( Regs[ OP_B ] ), Divisor ) );
	}
	DISPATCH();
LNegInt:
	{
		Int64 R;
		if ( __builtin_sub_overflow( (Int64)0, AsInt( Regs[ OP_B ] ), &R ) )
		{
			SAVE_CURSOR();
			return VM_ERR_OVERFLOW;
		}
		Regs[ OP_A ] = MakeInt( R );
	}
	DISPATCH();

	/* ---- float arithmetic ----------------------------------------------- */
LAddF64:
	Regs[ OP_A ] = MakeFloat( AsF64( Regs[ OP_B ] ) + AsF64( Regs[ OP_C ] ) );
	DISPATCH();
LSubF64:
	Regs[ OP_A ] = MakeFloat( AsF64( Regs[ OP_B ] ) - AsF64( Regs[ OP_C ] ) );
	DISPATCH();
LMulF64:
	Regs[ OP_A ] = MakeFloat( AsF64( Regs[ OP_B ] ) * AsF64( Regs[ OP_C ] ) );
	DISPATCH();
LDivF64:
	Regs[ OP_A ] = MakeFloat( AsF64( Regs[ OP_B ] ) / AsF64( Regs[ OP_C ] ) );
	DISPATCH();
LNegF64:
	Regs[ OP_A ] = MakeFloat( -AsF64( Regs[ OP_B ] ) );
	DISPATCH();

	/* ---- comparison ----------------------------------------------------- */
LLtInt:
	Regs[ OP_A ] = MakeBool( AsInt( Regs[ OP_B ] ) <  AsInt( Regs[ OP_C ] ) );
	DISPATCH();
LLeInt:
	Regs[ OP_A ] = MakeBool( AsInt( Regs[ OP_B ] ) <= AsInt( Regs[ OP_C ] ) );
	DISPATCH();
LGtInt:
	Regs[ OP_A ] = MakeBool( AsInt( Regs[ OP_B ] ) >  AsInt( Regs[ OP_C ] ) );
	DISPATCH();
LGeInt:
	Regs[ OP_A ] = MakeBool( AsInt( Regs[ OP_B ] ) >= AsInt( Regs[ OP_C ] ) );
	DISPATCH();
LLtF64:
	Regs[ OP_A ] = MakeBool( AsF64( Regs[ OP_B ] ) <  AsF64( Regs[ OP_C ] ) );
	DISPATCH();
LLeF64:
	Regs[ OP_A ] = MakeBool( AsF64( Regs[ OP_B ] ) <= AsF64( Regs[ OP_C ] ) );
	DISPATCH();
LGtF64:
	Regs[ OP_A ] = MakeBool( AsF64( Regs[ OP_B ] ) >  AsF64( Regs[ OP_C ] ) );
	DISPATCH();
LGeF64:
	Regs[ OP_A ] = MakeBool( AsF64( Regs[ OP_B ] ) >= AsF64( Regs[ OP_C ] ) );
	DISPATCH();
LEqInt:
	Regs[ OP_A ] = MakeBool( AsInt( Regs[ OP_B ] ) == AsInt( Regs[ OP_C ] ) );
	DISPATCH();
LNeInt:
	Regs[ OP_A ] = MakeBool( AsInt( Regs[ OP_B ] ) != AsInt( Regs[ OP_C ] ) );
	DISPATCH();

	/* ---- bitwise -------------------------------------------------------- */
LAndInt:
	Regs[ OP_A ] = MakeInt( AsInt( Regs[ OP_B ] ) & AsInt( Regs[ OP_C ] ) );
	DISPATCH();
LOrInt:
	Regs[ OP_A ] = MakeInt( AsInt( Regs[ OP_B ] ) | AsInt( Regs[ OP_C ] ) );
	DISPATCH();
LXorInt:
	Regs[ OP_A ] = MakeInt( AsInt( Regs[ OP_B ] ) ^ AsInt( Regs[ OP_C ] ) );
	DISPATCH();
LNotInt:
	Regs[ OP_A ] = MakeInt( ~AsInt( Regs[ OP_B ] ) );
	DISPATCH();

	/* ---- logical / conversion ------------------------------------------- */
LNot:
	Regs[ OP_A ] = MakeBool( !IsTruthy( Regs[ OP_B ] ) );
	DISPATCH();
LConvInt:
	{
		Int64 Val   = AsInt( Regs[ OP_A ] );
		Int32 Shift = 64 - OP_BX;
		Regs[ OP_A ]  = MakeInt( ( Val << Shift ) >> Shift );
	}
	DISPATCH();

	/* ---- immediate superinstructions ------------------------------------ */
LAddIntImm:
	{
		Int64 R;
		if ( __builtin_add_overflow( AsInt( Regs[ OP_B ] ), (Int64)OP_C, &R ) )
		{
			SAVE_CURSOR();
			return VM_ERR_OVERFLOW;
		}
		Regs[ OP_A ] = MakeInt( R );
	}
	DISPATCH();
LSubIntImm:
	{
		Int64 R;
		if ( __builtin_sub_overflow( AsInt( Regs[ OP_B ] ), (Int64)OP_C, &R ) )
		{
			SAVE_CURSOR();
			return VM_ERR_OVERFLOW;
		}
		Regs[ OP_A ] = MakeInt( R );
	}
	DISPATCH();
LEqIntImm:
	Regs[ OP_A ] = MakeBool( AsInt( Regs[ OP_B ] ) == (Int64)OP_C );
	DISPATCH();
LNeIntImm:
	Regs[ OP_A ] = MakeBool( AsInt( Regs[ OP_B ] ) != (Int64)OP_C );
	DISPATCH();

	/* ---- control flow --------------------------------------------------- */
LJmp:
	Ip = OP_BX;
	DISPATCH();
LJmpIfFalse:
	if ( !IsTruthy( Regs[ OP_A ] ) )
	{
		Ip = OP_BX;
	}
	DISPATCH();

	/* ---- compare+branch fusion -----------------------------------------
	 * When the comparison is TRUE the branch is not taken: skip the trailing
	 * (untouched) JMP_IF_FALSE. When FALSE, jump to that JMP_IF_FALSE's Bx. */
LBrLtInt:
	if ( AsInt( Regs[ OP_B ] ) <  AsInt( Regs[ OP_C ] ) ) { Ip += 1; } else { Ip = (Int32)( Code[ Ip ] & 0xFFFF ); }
	DISPATCH();
LBrLeInt:
	if ( AsInt( Regs[ OP_B ] ) <= AsInt( Regs[ OP_C ] ) ) { Ip += 1; } else { Ip = (Int32)( Code[ Ip ] & 0xFFFF ); }
	DISPATCH();
LBrGtInt:
	if ( AsInt( Regs[ OP_B ] ) >  AsInt( Regs[ OP_C ] ) ) { Ip += 1; } else { Ip = (Int32)( Code[ Ip ] & 0xFFFF ); }
	DISPATCH();
LBrGeInt:
	if ( AsInt( Regs[ OP_B ] ) >= AsInt( Regs[ OP_C ] ) ) { Ip += 1; } else { Ip = (Int32)( Code[ Ip ] & 0xFFFF ); }
	DISPATCH();
LBrEqInt:
	if ( AsInt( Regs[ OP_B ] ) == AsInt( Regs[ OP_C ] ) ) { Ip += 1; } else { Ip = (Int32)( Code[ Ip ] & 0xFFFF ); }
	DISPATCH();
LBrNeInt:
	if ( AsInt( Regs[ OP_B ] ) != AsInt( Regs[ OP_C ] ) ) { Ip += 1; } else { Ip = (Int32)( Code[ Ip ] & 0xFFFF ); }
	DISPATCH();
LBrLtIntImm:
	if ( AsInt( Regs[ OP_B ] ) <  (Int64)OP_C ) { Ip += 1; } else { Ip = (Int32)( Code[ Ip ] & 0xFFFF ); }
	DISPATCH();
LBrLeIntImm:
	if ( AsInt( Regs[ OP_B ] ) <= (Int64)OP_C ) { Ip += 1; } else { Ip = (Int32)( Code[ Ip ] & 0xFFFF ); }
	DISPATCH();
LBrGtIntImm:
	if ( AsInt( Regs[ OP_B ] ) >  (Int64)OP_C ) { Ip += 1; } else { Ip = (Int32)( Code[ Ip ] & 0xFFFF ); }
	DISPATCH();
LBrGeIntImm:
	if ( AsInt( Regs[ OP_B ] ) >= (Int64)OP_C ) { Ip += 1; } else { Ip = (Int32)( Code[ Ip ] & 0xFFFF ); }
	DISPATCH();
LBrEqIntImm:
	if ( AsInt( Regs[ OP_B ] ) == (Int64)OP_C ) { Ip += 1; } else { Ip = (Int32)( Code[ Ip ] & 0xFFFF ); }
	DISPATCH();
LBrNeIntImm:
	if ( AsInt( Regs[ OP_B ] ) != (Int64)OP_C ) { Ip += 1; } else { Ip = (Int32)( Code[ Ip ] & 0xFFFF ); }
	DISPATCH();

	/* ---- module globals ------------------------------------------------- */
LLoadGlobal:
	Regs[ OP_A ] = Ctx->Globals[ OP_BX ];
	DISPATCH();
LStoreGlobal:
	Ctx->Globals[ OP_BX ] = Regs[ OP_A ];
	DISPATCH();

LInitObj:
	{
		Ctx->AllocObject( Ctx->UserData, (Int32)OP_BX, &Regs[ OP_A ] );
	}
	DISPATCH();

LLoadField:
	{
		FHeapObject* Obj = (FHeapObject*)Regs[ OP_B ].Payload;
		Regs[ OP_A ] = Obj->Fields[ OP_C ];
	}
	DISPATCH();

LStoreField:
	{
		FHeapObject* Obj = (FHeapObject*)Regs[ OP_A ].Payload;
		Obj->Fields[ OP_B ] = Regs[ OP_C ];
	}
	DISPATCH();

LCopyBlock:
	{
		Int32 Count = OP_C;
		Int32 Dest  = OP_A;
		Int32 Src   = OP_B;
		for ( Int32 I = 0; I < Count; ++I )
		{
			Regs[ Dest + I ] = Regs[ Src + I ];
		}
	}
	DISPATCH();

LNewStruct:
	{
		Int32 Count = OP_BX;
		Int32 Dest  = OP_A;
		for ( Int32 I = 0; I < Count; ++I )
		{
			Regs[ Dest + I ] = MakeNil();
		}
	}
	DISPATCH();

LNop:
	DISPATCH();

	/* ---- calls ---------------------------------------------------------- */
LCall:
	{
		Int32 CalleeIndex = OP_B;
		Int32 CalleeBase  = Base + OP_A + 1;
		const FChunkInfo* Callee = &Chunks[ CalleeIndex ];

		if ( CalleeBase + Callee->NumRegisters > Ctx->StackCapacity ||
		     FrameDepth >= Ctx->FrameCap )
		{
			SAVE_CURSOR();
			return VM_ERR_STACKOVERFLOW;
		}

		/* Push resume state (Ip already points past the CALL). */
		Ctx->Frames[ FrameDepth ].ChunkIndex = CurChunk;
		Ctx->Frames[ FrameDepth ].Base       = Base;
		Ctx->Frames[ FrameDepth ].Ip         = Ip;
		FrameDepth += 1;

		/* Enter callee: args are already contiguous at Regs[A+1..]. */
		CurChunk = CalleeIndex;
		Base     = CalleeBase;
		REBIND();
		StackTop = Base + Callee->NumRegisters;
		Ip       = 0;

		/* Nil-init recycled RAII registers. */
		for ( Int32 K = 0; K < Callee->RaiiRegsCount; ++K )
		{
			Regs[ Callee->RaiiRegs[ K ] ] = MakeNil();
		}
	}
	DISPATCH();

LRet:
	{
		/* Early-return with live RAII objects must run drops in Crystal. */
		if ( Chunks[ CurChunk ].HasDropMap && Ip < Size )
		{
			Ip -= 1;               /* rewind to point at the RET */
			SAVE_CURSOR();
			Ctx->ColdOp = (Int32)Op;
			return VM_CALLBACK;
		}

		Int32 Slots = OP_BX > 0 ? OP_BX : 1;
		Int32 RetA  = OP_A;
		for ( Int32 J = 0; J < Slots; ++J )
		{
			Stack[ Base - 1 + J ] = Regs[ RetA + J ];
		}

		if ( FrameDepth == 0 )
		{
			Ctx->ResultSlots = Slots;
			SAVE_CURSOR();
			return VM_HALT;
		}

		FrameDepth -= 1;
		CurChunk = Ctx->Frames[ FrameDepth ].ChunkIndex;
		Base     = Ctx->Frames[ FrameDepth ].Base;
		Ip       = Ctx->Frames[ FrameDepth ].Ip;
		REBIND();
		StackTop = Base + Chunks[ CurChunk ].NumRegisters;
	}
	DISPATCH();

	/* ---- fell off end without RET: implicit nil result ------------------ */
LFellOff:
	Stack[ Base - 1 ] = MakeNil();
	if ( FrameDepth == 0 )
	{
		Ctx->ResultSlots = 1;
		SAVE_CURSOR();
		return VM_HALT;
	}
	FrameDepth -= 1;
	CurChunk = Ctx->Frames[ FrameDepth ].ChunkIndex;
	Base     = Ctx->Frames[ FrameDepth ].Base;
	Ip       = Ctx->Frames[ FrameDepth ].Ip;
	REBIND();
	StackTop = Base + Chunks[ CurChunk ].NumRegisters;
	DISPATCH();

	/* ---- cold opcode: hand back to the Crystal driver ------------------- */
LCold:
	Ip -= 1;                       /* rewind to point at the cold instruction */
	SAVE_CURSOR();
	Ctx->ColdOp = (Int32)Op;
	return VM_CALLBACK;

LBadOp:
	Ip -= 1;
	SAVE_CURSOR();
	Ctx->ColdOp = (Int32)Op;
	return VM_ERR_BADOP;

#undef SAVE_CURSOR
#undef REBIND
#undef DISPATCH
#undef OP_A
#undef OP_B
#undef OP_C
#undef OP_BX
}
