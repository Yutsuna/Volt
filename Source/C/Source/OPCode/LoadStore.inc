/* ---- load, move, store, and struct/block copy handlers ------------------- */

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

LLoadGlobal:
	Regs[ OP_A ] = Ctx->Globals[ OP_BX ];
	DISPATCH();

LStoreGlobal:
	Ctx->Globals[ OP_BX ] = Regs[ OP_A ];
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
