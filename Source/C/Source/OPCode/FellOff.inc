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
