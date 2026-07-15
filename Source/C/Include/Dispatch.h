#ifndef VOLT_DISPATCH_H
#define VOLT_DISPATCH_H

#include "Types.h"

/**
 * Runtime value — mirror of `IR::Value` { UInt8 tag; Void* payload }.
 * 16 bytes, Payload at offset 8 (verified by ABI spike). Payload holds the raw
 * bit pattern: an Int/Float's bits, or an undisguised heap pointer (never
 * OR'd with tag bits — the conservative GC must still see real pointers).
 */
typedef struct
{
	UInt8   Tag;
	Void*   Payload;
} FValue;

/** Tag ordinals — MUST match `IR::Value::Tag` declaration order. */
enum EValueTag
{
	VAL_INT    = 0,
	VAL_FLOAT  = 1,
	VAL_BOOL   = 2,
	VAL_REGEX  = 3,
	VAL_NIL    = 4,
	VAL_OBJECT = 5,
	VAL_PTR    = 6
};

enum EPtrWidth
{
	WIDTH_I8  = 0,
	WIDTH_I16 = 1,
	WIDTH_I32 = 2,
	WIDTH_I64 = 3,
	WIDTH_U8  = 4,
	WIDTH_U16 = 5,
	WIDTH_U32 = 6,
	WIDTH_U64 = 7,
	WIDTH_PTR = 8,
	WIDTH_F32 = 9,
	WIDTH_F64 = 10
};

/**
 * Flat heap object — mirror of the raw block `IR::HeapRef` allocates
 * (one scanned GC.malloc: header + inline field slots, no Crystal class
 * header anywhere near it). An Object-tagged FValue's Payload points at
 * the block start. `ClassRef` is the stable FRClass box wired at
 * allocation (null for an unregistered type_id — hand-built spec chunks),
 * so field access and virtual dispatch never consult a registry.
 */
typedef struct
{
	Int32 TypeId;      /* offset 0                                     */
	UInt8 Destroyed;   /* offset 4 (3 bytes padding follow)            */
	Void*   ClassRef;    /* offset 8  — FRClass*, null if unregistered   */
	FValue  Fields[];    /* offset 16 — SlotCount tagged values          */
} FHeapObject;

/**
 * Per-class runtime metadata — mirror of `LibVoltDispatch::RClass`.
 * One STABLE box per class: allocated once by the Crystal driver, never
 * moved, mutated in place on REPL re-registration (object headers point
 * straight at it). `VTable` entries and `DtorIndex` are chunk *indices*
 * (never FChunkInfo pointers — the chunk table reallocates on growth);
 * -1 = absent.
 */
typedef struct
{
	Int32        TypeId;
	Int32        SlotCount;
	Int32        DtorIndex;     /* synthesized `__dtor` chunk, -1 = none */
	Int32        VTableSize;
	const Int32* VTable;        /* callee chunk index per vtable slot    */
} FRClass;

/**
 * Monomorphic call-site type feedback, keyed by the CALL_METHOD
 * instruction's ip. Written by the interpreter, never read for dispatch
 * at this tier — it is the guard input for the Tier-1 JIT (mono site ⇒
 * class-id check + direct call). Owned by the per-Vm chunk table and
 * wiped whenever that table is rebuilt (chunk growth / REPL extend), so
 * stale observations never survive a class redefinition. Single-threaded
 * by construction; if OS threads ever enter the VM this moves into
 * per-context storage.
 */
typedef struct
{
	Int32  LastClassId;  /* last observed receiver class            */
	UInt32 HitCount;     /* monomorphic streak; 0 = no observation  */
} FCallFeedback;

/**
 * One pre-threaded instruction: the GCC computed-goto handler address plus
 * the original 32-bit operand word. 16 bytes so `Ip` indexes it directly.
 */
typedef struct
{
	Void*  Target;  /** handler label address (GDispatchTable[opcode]) */
	UInt32 Ins;     /** original instruction word [op:8|A:8|B:8|C:8]   */
	UInt32 _Pad;
} FThreadedIns;

/**
 * Chunk metadata, one per `@unit.chunks` entry. Built once by the Crystal
 * binding from immutable chunk fields, so the C core can resolve CALL targets
 * and enter callee frames without touching the Crystal object graph.
 */
typedef struct
{
	const UInt32*   Code;          /** chunk.code.to_unsafe (Instruction ≡ UInt32) */
	Int32           CodeSize;      /** chunk.code.size                              */
	const FValue*   Consts;        /** chunk.constants.to_unsafe                    */
	Int32           NumRegisters;  /** frame window size                            */
	const UInt8*    RaiiRegs;      /** registers to nil-init on frame entry         */
	Int32           RaiiRegsCount;
	UInt8           HasDropMap;    /** 1 => early-return RET must bounce to Crystal  */
	FCallFeedback*  Feedback;      /** ip-keyed call-site feedback, null if no CALL_METHOD */
	const FThreadedIns* ThreadedCode;  /** never null: CodeSize+1 entries, last = LFellOff sentinel */
	Int32               ThreadedSize;  /** logical instruction count (== CodeSize, excl. sentinel)  */
} FChunkInfo;

/**
 * One suspended caller on the shared frame stack — mirror of Vm's `CallFrame`
 * (chunk, base, ip) but with the chunk stored as an *index* into the chunk
 * table so both C and Crystal can walk it (Crystal needs the real Chunk for
 * RAII unwind on exception).
 */
typedef struct
{
	Int32 ChunkIndex;
	Int32 Base;
	Int32 Ip;          /** caller's post-CALL resume instruction pointer */
} FCallFrame;

/**
 * Mutable execution cursor + environment handed across the FFI boundary. The
 * Crystal driver fills the environment pointers once, then loops:
 * call Volt_Dispatch, service any VM_CALLBACK by advancing this same struct,
 * re-enter — until VM_HALT or an error.
 */
typedef struct
{
	/* --- environment (stable for the whole execute) --- */
	const FChunkInfo* Chunks;
	Int32             ChunkCount;
	FValue*           Stack;
	Int32             StackCapacity;
	FValue*           Globals;
	Int32             GlobalCount;
	FCallFrame*       Frames;
	Int32             FrameCap;

	/* --- cursor (advanced by C and by the Crystal callback handler) --- */
	Int32           CurChunk;    /** index of the executing chunk        */
	Int32           Base;        /** current frame base into Stack        */
	Int32           Ip;          /** current instruction pointer          */
	Int32           FrameDepth;  /** live entries in Frames               */
	Int32           StackTop;    /** first free slot above active frames  */

	/* --- outputs --- */
	Int32           ColdOp;      /** opcode value at Ip when VM_CALLBACK   */
	Int32           ResultSlots; /** slot count returned on VM_HALT        */

	/* --- object model (Phase C-runtime: classes visible to C) --- */
	FRClass* const*   ClassIndex;  /** type_id -> stable box, null holes (JIT-phase consumer) */
	Int32             ClassCount;  /** ClassIndex length                     */

	/**
	 * Allocation stays in Crystal (Boehm-registered, scanned) but is entered
	 * through this direct function pointer instead of a full VM_CALLBACK
	 * bounce. Contract: non-closure, MUST NOT raise (Dispatch.c has no
	 * exception tables), and the C core runs SAVE_CURSOR() before calling.
	 * Out-parameter instead of a struct return to stay off any ABI edge.
	 */
	Void*             UserData;    /** the Crystal Vm, passed back verbatim  */
	Void ( *AllocObject )( Void* UserData, Int32 TypeId, FValue* Out );
} FVmContext;

/** Return status of Volt_Dispatch. */
enum EVmStatus
{
	VM_HALT              = 0,  /** outermost frame returned; ResultSlots set     */
	VM_CALLBACK          = 1,  /** cold opcode at Ip needs Crystal (ColdOp set)  */
	VM_ERR_DIVZERO       = 2,  /** integer division/modulo by zero               */
	VM_ERR_STACKOVERFLOW = 3, /** callee window would exceed StackCapacity      */
	VM_ERR_BADOP         = 4,  /** unknown/unencodable opcode                    */
	VM_ERR_OVERFLOW      = 5,  /** checked integer arithmetic overflowed         */
	VM_ERR_NILRECEIVER   = 6,  /** CALL_METHOD on a nil/non-object receiver      */
	VM_ERR_NOMETHOD      = 7,  /** CALL_METHOD: no class / unresolved vtable slot */
	VM_ERR_NULLPTR       = 8   /** dereferenced pointer is null                  */
};

/* --- ABI tripwires: any drift from the Crystal mirrors fails the build --- */
_Static_assert( sizeof( FValue ) == 16,                     "FValue must mirror IR::Value (16 bytes)" );
_Static_assert( offsetof( FValue, Payload ) == 8,           "FValue.Payload must sit at offset 8" );
_Static_assert( offsetof( FHeapObject, Destroyed ) == 4,    "FHeapObject.Destroyed must sit at offset 4" );
_Static_assert( offsetof( FHeapObject, ClassRef ) == 8,     "FHeapObject.ClassRef must sit at offset 8" );
_Static_assert( offsetof( FHeapObject, Fields ) == 16,      "FHeapObject.Fields must sit at offset 16 (HeapRef::HEADER_BYTES)" );
_Static_assert( sizeof( FRClass ) == 24,                    "FRClass must mirror LibVoltDispatch::RClass (24 bytes)" );
_Static_assert( sizeof( FCallFeedback ) == 8,               "FCallFeedback must stay 8 bytes (ip-indexed side array)" );
_Static_assert( sizeof( FChunkInfo ) == 72,                 "FChunkInfo must mirror LibVoltDispatch::ChunkInfo (72 bytes)" );
_Static_assert( offsetof( FVmContext, ClassIndex ) == 88,   "FVmContext.ClassIndex must sit at offset 88" );
_Static_assert( offsetof( FVmContext, AllocObject ) == 112, "FVmContext.AllocObject must sit at offset 112" );
_Static_assert( sizeof( FVmContext ) == 120,                "FVmContext must mirror LibVoltDispatch::VmContext (120 bytes)" );

/**
 * Runs the threaded core from Ctx->CurChunk/Base/Ip until it halts, hits a
 * cold opcode, or errors. Returns an EVmStatus; on VM_CALLBACK the caller
 * services Ctx->ColdOp (mutating the cursor) and calls again.
 *
 * Ctx == NULL is an init-only entry: the label-address tables are populated
 * and VM_HALT is returned without executing anything (used by
 * Volt_ThreadChunk before the first real dispatch).
 */
Int32 Volt_Dispatch( FVmContext* Ctx );

/**
 * Pre-thread a bytecode array for direct-threaded dispatch. OutThreaded must
 * point to CodeSize+1 slots: the extra slot receives the LFellOff sentinel
 * that replaces the per-instruction bounds check in the hot loop.
 */
void Volt_ThreadChunk( const UInt32* Code, Int32 CodeSize, FThreadedIns* OutThreaded );

#endif /* VOLT_DISPATCH_H */
