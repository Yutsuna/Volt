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
		DispatchTable[ 24 ] = &&LEq;          /* EQ            */
		DispatchTable[ 25 ] = &&LNe;          /* NE            */
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
		DispatchTable[ 48 ] = &&LDrop;        /* DROP          */
		DispatchTable[ 49 ] = &&LCold;        /* DROP_SCOPE    */
		DispatchTable[ 50 ] = &&LCold;        /* RAISE         */
		DispatchTable[ 51 ] = &&LInitObj;     /* INIT_OBJ      */
		DispatchTable[ 52 ] = &&LLoadField;   /* LOAD_FIELD    */
		DispatchTable[ 53 ] = &&LStoreField;  /* STORE_FIELD   */
		DispatchTable[ 54 ] = &&LCallMethod;  /* CALL_METHOD   */
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

#include "OPCode/LoadStore.inc"
#include "OPCode/Arith.inc"
#include "OPCode/Compare.inc"
#include "OPCode/ControlFlow.inc"
#include "OPCode/Raii.inc"
#include "OPCode/Call.inc"
#include "OPCode/Nop.inc"
#include "OPCode/FellOff.inc"
#include "OPCode/Cold.inc"

#undef SAVE_CURSOR
#undef REBIND
#undef DISPATCH
#undef OP_A
#undef OP_B
#undef OP_C
#undef OP_BX
}
