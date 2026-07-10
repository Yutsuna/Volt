/* ---- method and function call and return handlers ------------------------ */

LCallMethod:
	{
		FValue Recv = Regs[ OP_A + 1 ];
		if ( Recv.Tag != VAL_OBJECT || Recv.Payload == NULL )
		{
			Ip -= 1;
			SAVE_CURSOR();
			return VM_ERR_NILRECEIVER;
		}

		FHeapObject* Obj = (FHeapObject*)Recv.Payload;
		FRClass* Class = (FRClass*)Obj->ClassRef;
		if ( Class == NULL || OP_B >= Class->VTableSize || Class->VTable[ OP_B ] < 0 )
		{
			Ip -= 1;
			SAVE_CURSOR();
			return VM_ERR_NOMETHOD;
		}

		/* Record call-site type feedback (inline caching) */
		if ( Chunks[ CurChunk ].Feedback != NULL )
		{
			FCallFeedback* IC = &Chunks[ CurChunk ].Feedback[ Ip - 1 ];
			Int32 ClassId = Obj->TypeId;
			if ( IC->LastClassId == ClassId )
			{
				IC->HitCount++;
			}
			else
			{
				if ( IC->HitCount == 0 )
				{
					IC->LastClassId = ClassId;
					IC->HitCount = 1;
				}
				else
				{
					IC->LastClassId = -1; /* invalidated monomorphic status (polymorphic) */
				}
			}
		}

		Int32 CalleeIndex = Class->VTable[ OP_B ];
		Int32 CalleeBase  = Base + OP_A + 1;
		const FChunkInfo* Callee = &Chunks[ CalleeIndex ];

		if ( CalleeBase + Callee->NumRegisters > Ctx->StackCapacity ||
		     FrameDepth >= Ctx->FrameCap )
		{
			SAVE_CURSOR();
			return VM_ERR_STACKOVERFLOW;
		}

		/* Push resume state (Ip already points past the CALL_METHOD). */
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
			Ctx->ColdOp = (Int32)( Ins >> 24 );
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
