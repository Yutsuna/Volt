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

/* File-scope copy of the function-local dispatch table, used by
   Volt_ThreadChunk to pre-thread bytecode outside Volt_Dispatch. */
static Void* GDispatchTable[ 256 ];
static int   GTableReady = 0;

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
		DispatchTable[ 44 ] = &&LInitObj;     /* INIT          */
		DispatchTable[ 45 ] = &&LDrop;        /* DROP          */
		DispatchTable[ 46 ] = &&LCold;        /* DROP_SCOPE    */
		DispatchTable[ 47 ] = &&LCold;        /* RAISE         */
		DispatchTable[ 48 ] = &&LInitObj;     /* INIT_OBJ      */
		DispatchTable[ 49 ] = &&LLoadField;   /* LOAD_FIELD    */
		DispatchTable[ 50 ] = &&LStoreField;  /* STORE_FIELD   */
		DispatchTable[ 51 ] = &&LCallMethod;  /* CALL_METHOD   */
		DispatchTable[ 52 ] = &&LCold;        /* CALL_MIXIN    */
		DispatchTable[ 53 ] = &&LCopyBlock;   /* COPY_BLOCK    */
		DispatchTable[ 54 ] = &&LNewStruct;   /* NEW_STRUCT    */
		DispatchTable[ 55 ] = &&LLoadGlobal;
		DispatchTable[ 56 ] = &&LStoreGlobal;
		DispatchTable[ 57 ] = &&LNop;
		DispatchTable[ 58 ] = &&LAddIntImm;
		DispatchTable[ 59 ] = &&LSubIntImm;
		DispatchTable[ 60 ] = &&LEqIntImm;
		DispatchTable[ 61 ] = &&LNeIntImm;
		DispatchTable[ 62 ] = &&LBrLtInt;
		DispatchTable[ 63 ] = &&LBrLeInt;
		DispatchTable[ 64 ] = &&LBrGtInt;
		DispatchTable[ 65 ] = &&LBrGeInt;
		DispatchTable[ 66 ] = &&LBrEqInt;
		DispatchTable[ 67 ] = &&LBrNeInt;
		DispatchTable[ 68 ] = &&LBrLtIntImm;
		DispatchTable[ 69 ] = &&LBrLeIntImm;
		DispatchTable[ 70 ] = &&LBrGtIntImm;
		DispatchTable[ 71 ] = &&LBrGeIntImm;
		DispatchTable[ 72 ] = &&LBrEqIntImm;
		DispatchTable[ 73 ] = &&LBrNeIntImm;
		DispatchTable[ 74 ] = &&LLoadPtr;
		DispatchTable[ 75 ] = &&LStorePtr;
		DispatchTable[ 76 ] = &&LPtrAdd;
		DispatchTable[ 77 ] = &&LPtrSub;
		DispatchTable[ 78 ] = &&LAddrLocal;
		DispatchTable[ 79 ] = &&LAddrField;
		for ( int I = 0; I < 256; ++I )
			GDispatchTable[ I ] = DispatchTable[ I ];
		GTableReady = 1;
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
	Int32           Size   = Chunks[ CurChunk ].ThreadedCode ? Chunks[ CurChunk ].ThreadedSize : Chunks[ CurChunk ].CodeSize;
	const FValue*     Consts = Chunks[ CurChunk ].Consts;
	const UInt64*     ThreadedCode = Chunks[ CurChunk ].ThreadedCode;
	FValue*           Regs   = Stack + Base;

	UInt32 Ins = 0;
	UInt32 Op  = 0;

#define SAVE_CURSOR()             \
	Ctx->CurChunk   = CurChunk;   \
	Ctx->Base       = Base;       \
	Ctx->Ip         = Ip;         \
	Ctx->FrameDepth = FrameDepth; \
	Ctx->StackTop   = StackTop

#define REBIND()                                    \
	Code         = Chunks[ CurChunk ].Code;         \
	Size         = Chunks[ CurChunk ].ThreadedCode ? Chunks[ CurChunk ].ThreadedSize : Chunks[ CurChunk ].CodeSize; \
	Consts       = Chunks[ CurChunk ].Consts;       \
	ThreadedCode = Chunks[ CurChunk ].ThreadedCode; \
	Regs         = Stack + Base

#define DISPATCH()                                      \
	do {                                                \
		if ( Ip >= Size ) goto LFellOff;                \
		if ( ThreadedCode )                             \
		{                                               \
			Ins = (UInt32)ThreadedCode[ Ip * 2 + 1 ];   \
			Op  = Ins >> 24;                            \
			{                                           \
				Void* Target = (Void*)ThreadedCode[ Ip * 2 ]; \
				Ip += 1;                                \
				goto *Target;                           \
			}                                           \
		}                                               \
		Ins = Code[ Ip++ ];                             \
		Op  = Ins >> 24;                                \
		goto *DispatchTable[ Op ];                      \
	} while ( 0 )

/* Operand accessors on the current instruction word. */
#define OP_A  ( (Int32)( ( Ins >> 16 ) & 0xFF ) )
#define OP_B  ( (Int32)( ( Ins >> 8 ) & 0xFF ) )
#define OP_C  ( (Int32)( Ins & 0xFF ) )
#define OP_BX ( (Int32)( Ins & 0xFFFF ) )
#define PEEK_BX_AT_IP() \
	( (Int32)( ( ( ThreadedCode ) ? (UInt32)ThreadedCode[ Ip * 2 + 1 ] : Code[ Ip ] ) & 0xFFFF ) )

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
#include "OPCode/Pointer.inc"

#undef SAVE_CURSOR
#undef REBIND
#undef DISPATCH
#undef OP_A
#undef OP_B
#undef OP_C
#undef OP_BX
#undef PEEK_BX_AT_IP
}

/* ---------------------------------------------------------------------------
 * Pre-thread a bytecode array so Volt_Dispatch can use direct-threaded goto.
 * OutThreaded must have room for CodeSize * 2 UInt64 entries.
 * Layout: [ handler_0, ins_0, handler_1, ins_1, ... ]
 * ------------------------------------------------------------------------- */
void Volt_ThreadChunk( const UInt32* Code, Int32 CodeSize, UInt64* OutThreaded )
{
	/* If the dispatch table hasn't been initialised yet, force a single
	   no-op dispatch pass to populate it.  This happens at most once. */
	if ( !GTableReady )
	{
		/* Build a minimal dummy context with a 1-instruction RET chunk. */
		UInt32 DummyCode[1];
		DummyCode[0] = ( (UInt32)33 /* RET */ ) << 24;  /* RET 0,0 */
		FValue DummyStack[4];
		for ( int I = 0; I < 4; ++I ) { DummyStack[I].Tag = VAL_NIL; DummyStack[I].Payload = (Void*)0; }
		FChunkInfo DummyChunk;
		DummyChunk.Code          = DummyCode;
		DummyChunk.CodeSize      = 1;
		DummyChunk.Consts        = NULL;
		DummyChunk.NumRegisters  = 4;
		DummyChunk.RaiiRegs      = NULL;
		DummyChunk.RaiiRegsCount = 0;
		DummyChunk.HasDropMap    = 0;
		DummyChunk.Feedback      = NULL;
		DummyChunk.ThreadedCode  = NULL;
		DummyChunk.ThreadedSize  = 0;
		FCallFrame DummyFrames[1];
		FVmContext DummyCtx;
		DummyCtx.Chunks        = &DummyChunk;
		DummyCtx.ChunkCount    = 1;
		DummyCtx.Stack         = DummyStack;
		DummyCtx.StackCapacity = 4;
		DummyCtx.Globals       = NULL;
		DummyCtx.GlobalCount   = 0;
		DummyCtx.Frames        = DummyFrames;
		DummyCtx.FrameCap      = 1;
		DummyCtx.CurChunk      = 0;
		DummyCtx.Base          = 1;
		DummyCtx.Ip            = 0;
		DummyCtx.FrameDepth    = 0;
		DummyCtx.StackTop      = 4;
		DummyCtx.ColdOp        = 0;
		DummyCtx.ResultSlots   = 0;
		DummyCtx.ClassIndex    = NULL;
		DummyCtx.ClassCount    = 0;
		DummyCtx.UserData      = NULL;
		DummyCtx.AllocObject   = NULL;
		Volt_Dispatch( &DummyCtx );
	}

	for ( Int32 I = 0; I < CodeSize; ++I )
	{
		UInt32 Ins = Code[ I ];
		UInt32 Op  = Ins >> 24;
		OutThreaded[ I * 2 ]     = (UInt64)(IntPtr)GDispatchTable[ Op ];
		OutThreaded[ I * 2 + 1 ] = (UInt64)Ins;
	}
}
