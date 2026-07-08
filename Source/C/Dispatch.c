#include "Dispatch.h"

/* ---------------------------------------------------------------------------
 * FValue construction / access — bit-exact with IR::Value.
 * ------------------------------------------------------------------------- */

static inline FValue MakeInt( int64_t Value )
{
	FValue Result;
	Result.Tag     = VAL_INT;
	Result.Payload = (void*)(intptr_t)Value;
	return Result;
}

static inline FValue MakeBool( int Value )
{
	FValue Result;
	Result.Tag     = VAL_BOOL;
	Result.Payload = (void*)(intptr_t)( Value ? 1 : 0 );
	return Result;
}

static inline FValue MakeFloat( double Value )
{
	union { double D; void* P; } Bits;
	FValue Result;
	Bits.D         = Value;
	Result.Tag     = VAL_FLOAT;
	Result.Payload = Bits.P;
	return Result;
}

static inline FValue MakeNil( void )
{
	FValue Result;
	Result.Tag     = VAL_NIL;
	Result.Payload = (void*)0;
	return Result;
}

static inline int64_t AsInt( FValue Value )
{
	return (int64_t)(intptr_t)Value.Payload;
}

static inline double AsF64( FValue Value )
{
	union { void* P; double D; } Bits;
	Bits.P = Value.Payload;
	return Bits.D;
}

/** Volt truthiness: only nil and false are falsy (mirror of Value#truthy?). */
static inline int IsTruthy( FValue Value )
{
	return !( Value.Tag == VAL_NIL || ( Value.Tag == VAL_BOOL && Value.Payload == (void*)0 ) );
}

/** Floored integer division / modulo — matches Crystal's `//` and `%`. */
static inline int64_t FloorDiv( int64_t A, int64_t B )
{
	int64_t Q = A / B;
	int64_t R = A % B;
	if ( ( R != 0 ) && ( ( R < 0 ) != ( B < 0 ) ) )
	{
		Q -= 1;
	}
	return Q;
}

static inline int64_t FloorMod( int64_t A, int64_t B )
{
	int64_t R = A % B;
	if ( ( R != 0 ) && ( ( R < 0 ) != ( B < 0 ) ) )
	{
		R += B;
	}
	return R;
}

/* ---------------------------------------------------------------------------
 * Dispatch core.
 * ------------------------------------------------------------------------- */

int32_t Volt_Dispatch( FVmContext* Ctx )
{
	/* One computed-goto table shared by every entry; label addresses are fixed
	 * for the program's lifetime, so it is filled exactly once. Crystal fibers
	 * are cooperative (no preemptive data race on Inited). */
	static void* DispatchTable[ 256 ];
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
		DispatchTable[ 47 ] = &&LCold;        /* INIT          */
		DispatchTable[ 48 ] = &&LCold;        /* DROP          */
		DispatchTable[ 49 ] = &&LCold;        /* DROP_SCOPE    */
		DispatchTable[ 50 ] = &&LCold;        /* RAISE         */
		DispatchTable[ 51 ] = &&LCold;        /* INIT_OBJ      */
		DispatchTable[ 52 ] = &&LCold;        /* LOAD_FIELD    */
		DispatchTable[ 53 ] = &&LCold;        /* STORE_FIELD   */
		DispatchTable[ 54 ] = &&LCold;        /* CALL_METHOD   */
		DispatchTable[ 55 ] = &&LCold;        /* CALL_MIXIN    */
		DispatchTable[ 56 ] = &&LCold;        /* COPY_BLOCK    */
		DispatchTable[ 57 ] = &&LCold;        /* NEW_STRUCT    */
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
	int32_t           CurChunk = Ctx->CurChunk;
	int32_t           Base     = Ctx->Base;
	int32_t           Ip       = Ctx->Ip;
	int32_t           FrameDepth = Ctx->FrameDepth;
	int32_t           StackTop = Ctx->StackTop;

	const uint32_t*   Code   = Chunks[ CurChunk ].Code;
	int32_t           Size   = Chunks[ CurChunk ].CodeSize;
	const FValue*     Consts = Chunks[ CurChunk ].Consts;
	FValue*           Regs   = Stack + Base;

	uint32_t Ins = 0;
	uint32_t Op  = 0;

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
#define OP_A  ( (int32_t)( ( Ins >> 16 ) & 0xFF ) )
#define OP_B  ( (int32_t)( ( Ins >> 8 ) & 0xFF ) )
#define OP_C  ( (int32_t)( Ins & 0xFF ) )
#define OP_BX ( (int32_t)( Ins & 0xFFFF ) )

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
		int64_t R;
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
		int64_t R;
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
		int64_t R;
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
		int64_t Divisor = AsInt( Regs[ OP_C ] );
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
		int64_t Divisor = AsInt( Regs[ OP_C ] );
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
		int64_t R;
		if ( __builtin_sub_overflow( (int64_t)0, AsInt( Regs[ OP_B ] ), &R ) )
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
		int64_t Val   = AsInt( Regs[ OP_A ] );
		int32_t Shift = 64 - OP_BX;
		Regs[ OP_A ]  = MakeInt( ( Val << Shift ) >> Shift );
	}
	DISPATCH();

	/* ---- immediate superinstructions ------------------------------------ */
LAddIntImm:
	{
		int64_t R;
		if ( __builtin_add_overflow( AsInt( Regs[ OP_B ] ), (int64_t)OP_C, &R ) )
		{
			SAVE_CURSOR();
			return VM_ERR_OVERFLOW;
		}
		Regs[ OP_A ] = MakeInt( R );
	}
	DISPATCH();
LSubIntImm:
	{
		int64_t R;
		if ( __builtin_sub_overflow( AsInt( Regs[ OP_B ] ), (int64_t)OP_C, &R ) )
		{
			SAVE_CURSOR();
			return VM_ERR_OVERFLOW;
		}
		Regs[ OP_A ] = MakeInt( R );
	}
	DISPATCH();
LEqIntImm:
	Regs[ OP_A ] = MakeBool( AsInt( Regs[ OP_B ] ) == (int64_t)OP_C );
	DISPATCH();
LNeIntImm:
	Regs[ OP_A ] = MakeBool( AsInt( Regs[ OP_B ] ) != (int64_t)OP_C );
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
	if ( AsInt( Regs[ OP_B ] ) <  AsInt( Regs[ OP_C ] ) ) { Ip += 1; } else { Ip = (int32_t)( Code[ Ip ] & 0xFFFF ); }
	DISPATCH();
LBrLeInt:
	if ( AsInt( Regs[ OP_B ] ) <= AsInt( Regs[ OP_C ] ) ) { Ip += 1; } else { Ip = (int32_t)( Code[ Ip ] & 0xFFFF ); }
	DISPATCH();
LBrGtInt:
	if ( AsInt( Regs[ OP_B ] ) >  AsInt( Regs[ OP_C ] ) ) { Ip += 1; } else { Ip = (int32_t)( Code[ Ip ] & 0xFFFF ); }
	DISPATCH();
LBrGeInt:
	if ( AsInt( Regs[ OP_B ] ) >= AsInt( Regs[ OP_C ] ) ) { Ip += 1; } else { Ip = (int32_t)( Code[ Ip ] & 0xFFFF ); }
	DISPATCH();
LBrEqInt:
	if ( AsInt( Regs[ OP_B ] ) == AsInt( Regs[ OP_C ] ) ) { Ip += 1; } else { Ip = (int32_t)( Code[ Ip ] & 0xFFFF ); }
	DISPATCH();
LBrNeInt:
	if ( AsInt( Regs[ OP_B ] ) != AsInt( Regs[ OP_C ] ) ) { Ip += 1; } else { Ip = (int32_t)( Code[ Ip ] & 0xFFFF ); }
	DISPATCH();
LBrLtIntImm:
	if ( AsInt( Regs[ OP_B ] ) <  (int64_t)OP_C ) { Ip += 1; } else { Ip = (int32_t)( Code[ Ip ] & 0xFFFF ); }
	DISPATCH();
LBrLeIntImm:
	if ( AsInt( Regs[ OP_B ] ) <= (int64_t)OP_C ) { Ip += 1; } else { Ip = (int32_t)( Code[ Ip ] & 0xFFFF ); }
	DISPATCH();
LBrGtIntImm:
	if ( AsInt( Regs[ OP_B ] ) >  (int64_t)OP_C ) { Ip += 1; } else { Ip = (int32_t)( Code[ Ip ] & 0xFFFF ); }
	DISPATCH();
LBrGeIntImm:
	if ( AsInt( Regs[ OP_B ] ) >= (int64_t)OP_C ) { Ip += 1; } else { Ip = (int32_t)( Code[ Ip ] & 0xFFFF ); }
	DISPATCH();
LBrEqIntImm:
	if ( AsInt( Regs[ OP_B ] ) == (int64_t)OP_C ) { Ip += 1; } else { Ip = (int32_t)( Code[ Ip ] & 0xFFFF ); }
	DISPATCH();
LBrNeIntImm:
	if ( AsInt( Regs[ OP_B ] ) != (int64_t)OP_C ) { Ip += 1; } else { Ip = (int32_t)( Code[ Ip ] & 0xFFFF ); }
	DISPATCH();

	/* ---- module globals ------------------------------------------------- */
LLoadGlobal:
	Regs[ OP_A ] = Ctx->Globals[ OP_BX ];
	DISPATCH();
LStoreGlobal:
	Ctx->Globals[ OP_BX ] = Regs[ OP_A ];
	DISPATCH();

LNop:
	DISPATCH();

	/* ---- calls ---------------------------------------------------------- */
LCall:
	{
		int32_t CalleeIndex = OP_B;
		int32_t CalleeBase  = Base + OP_A + 1;
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
		for ( int32_t K = 0; K < Callee->RaiiRegsCount; ++K )
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
			Ctx->ColdOp = (int32_t)Op;
			return VM_CALLBACK;
		}

		int32_t Slots = OP_BX > 0 ? OP_BX : 1;
		int32_t RetA  = OP_A;
		for ( int32_t J = 0; J < Slots; ++J )
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
	Ctx->ColdOp = (int32_t)Op;
	return VM_CALLBACK;

LBadOp:
	Ip -= 1;
	SAVE_CURSOR();
	Ctx->ColdOp = (int32_t)Op;
	return VM_ERR_BADOP;

#undef SAVE_CURSOR
#undef REBIND
#undef DISPATCH
#undef OP_A
#undef OP_B
#undef OP_C
#undef OP_BX
}
