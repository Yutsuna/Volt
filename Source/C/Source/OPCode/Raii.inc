/* ---- RAII object lifecycle handlers -------------------------------------- */

LInitObj:
	{
		Ctx->AllocObject( Ctx->UserData, (Int32)OP_BX, &Regs[ OP_A ] );
	}
	DISPATCH();

LDrop:
	{
		FValue Val = Regs[ OP_A ];
		if ( Val.Tag == VAL_OBJECT && Val.Payload != NULL )
		{
			FHeapObject* Obj = (FHeapObject*)Val.Payload;
			if ( !Obj->Destroyed )
			{
				FRClass* Class = (FRClass*)Obj->ClassRef;
				if ( Class != NULL && Class->DtorIndex >= 0 )
				{
					Int32 DtorIndex = Class->DtorIndex;
					const FChunkInfo* Dtor = &Chunks[ DtorIndex ];

					if ( FrameDepth >= Ctx->FrameCap )
					{
						SAVE_CURSOR();
						return VM_ERR_STACKOVERFLOW;
					}

					Ctx->Frames[ FrameDepth ].ChunkIndex = CurChunk;
					Ctx->Frames[ FrameDepth ].Base       = Base;
					Ctx->Frames[ FrameDepth ].Ip         = Ip;
					FrameDepth += 1;

					Int32 DtorBase = StackTop;
					if ( DtorBase + Dtor->NumRegisters > Ctx->StackCapacity )
					{
						SAVE_CURSOR();
						return VM_ERR_STACKOVERFLOW;
					}

					Stack[ DtorBase ] = Val;

					CurChunk = DtorIndex;
					Base     = DtorBase;
					REBIND();
					StackTop = Base + Dtor->NumRegisters;
					Ip       = 0;

					for ( Int32 K = 0; K < Dtor->RaiiRegsCount; ++K )
					{
						Regs[ Dtor->RaiiRegs[ K ] ] = MakeNil();
					}
				}

				Obj->Destroyed = 1;
			}
		}

		Regs[ OP_A ] = MakeNil();
	}
	DISPATCH();
