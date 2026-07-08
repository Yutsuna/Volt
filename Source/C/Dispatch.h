/**
 * Dispatch.h — C↔Crystal contract for the Volt Tier-0 direct-threaded core.
 *
 * The C shim owns the hot dispatch loop (computed-goto / labels-as-values) and
 * mutates Crystal's own data structures in place: no serialization crosses the
 * FFI boundary. Every type here is a bit-exact mirror of its Crystal
 * counterpart (Source/Volt/IR/Value.cr, Instruction.cr, Opcode.cr) — see the
 * ABI notes on each struct. The boundary is entered once per `execute` and
 * returns only on HALT, a cold opcode (VM_CALLBACK), or an error.
 *
 * Coding standard: Epic Games / Unreal C++ (PascalCase, Allman braces).
 */
#ifndef VOLT_DISPATCH_H
#define VOLT_DISPATCH_H

#include <stdint.h>

/**
 * Runtime value — mirror of `IR::Value` { UInt8 tag; Void* payload }.
 * 16 bytes, Payload at offset 8 (verified by ABI spike). Payload holds the raw
 * bit pattern: an Int/Float's bits, or an undisguised heap pointer (never
 * OR'd with tag bits — the conservative GC must still see real pointers).
 */
typedef struct
{
	uint8_t Tag;
	void*   Payload;
} FValue;

/** Tag ordinals — MUST match `IR::Value::Tag` declaration order. */
enum EValueTag
{
	VAL_INT    = 0,
	VAL_FLOAT  = 1,
	VAL_BOOL   = 2,
	VAL_STR    = 3,
	VAL_REGEX  = 4,
	VAL_NIL    = 5,
	VAL_OBJECT = 6
};

/**
 * Chunk metadata, one per `@unit.chunks` entry. Built once by the Crystal
 * binding from immutable chunk fields, so the C core can resolve CALL targets
 * and enter callee frames without touching the Crystal object graph.
 */
typedef struct
{
	const uint32_t* Code;          /** chunk.code.to_unsafe (Instruction ≡ UInt32) */
	int32_t         CodeSize;      /** chunk.code.size                              */
	const FValue*   Consts;        /** chunk.constants.to_unsafe                    */
	int32_t         NumRegisters;  /** frame window size                            */
	const uint8_t*  RaiiRegs;      /** registers to nil-init on frame entry         */
	int32_t         RaiiRegsCount;
	uint8_t         HasDropMap;    /** 1 => early-return RET must bounce to Crystal  */
} FChunkInfo;

/**
 * One suspended caller on the shared frame stack — mirror of Vm's `CallFrame`
 * (chunk, base, ip) but with the chunk stored as an *index* into the chunk
 * table so both C and Crystal can walk it (Crystal needs the real Chunk for
 * RAII unwind on exception).
 */
typedef struct
{
	int32_t ChunkIndex;
	int32_t Base;
	int32_t Ip;          /** caller's post-CALL resume instruction pointer */
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
	int32_t           ChunkCount;
	FValue*           Stack;
	int32_t           StackCapacity;
	FValue*           Globals;
	int32_t           GlobalCount;
	FCallFrame*       Frames;
	int32_t           FrameCap;

	/* --- cursor (advanced by C and by the Crystal callback handler) --- */
	int32_t           CurChunk;    /** index of the executing chunk        */
	int32_t           Base;        /** current frame base into Stack        */
	int32_t           Ip;          /** current instruction pointer          */
	int32_t           FrameDepth;  /** live entries in Frames               */
	int32_t           StackTop;    /** first free slot above active frames  */

	/* --- outputs --- */
	int32_t           ColdOp;      /** opcode value at Ip when VM_CALLBACK   */
	int32_t           ResultSlots; /** slot count returned on VM_HALT        */
} FVmContext;

/** Return status of Volt_Dispatch. */
enum EVmStatus
{
	VM_HALT             = 0,  /** outermost frame returned; ResultSlots set     */
	VM_CALLBACK         = 1,  /** cold opcode at Ip needs Crystal (ColdOp set)  */
	VM_ERR_DIVZERO      = 2,  /** integer division/modulo by zero               */
	VM_ERR_STACKOVERFLOW = 3, /** callee window would exceed StackCapacity      */
	VM_ERR_BADOP        = 4,  /** unknown/unencodable opcode                    */
	VM_ERR_OVERFLOW     = 5   /** checked integer arithmetic overflowed         */
};

/**
 * Runs the threaded core from Ctx->CurChunk/Base/Ip until it halts, hits a
 * cold opcode, or errors. Returns an EVmStatus; on VM_CALLBACK the caller
 * services Ctx->ColdOp (mutating the cursor) and calls again.
 */
int32_t Volt_Dispatch( FVmContext* Ctx );

#endif /* VOLT_DISPATCH_H */
