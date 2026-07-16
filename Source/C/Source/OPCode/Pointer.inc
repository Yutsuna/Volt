/* ---- pointer and memory operation handlers ------------------- */

LLoadPtr:
	{
		Void* Ptr = Regs[ OP_B ].Payload;
		if ( Ptr == NULL )
		{
			SAVE_CURSOR();
			return VM_ERR_NULLPTR;
		}
		switch ( OP_C )
		{
		case WIDTH_I8:
			Regs[ OP_A ] = MakeInt( *(int8_t*)Ptr );
			break;
		case WIDTH_I16:
			Regs[ OP_A ] = MakeInt( *(int16_t*)Ptr );
			break;
		case WIDTH_I32:
			Regs[ OP_A ] = MakeInt( *(int32_t*)Ptr );
			break;
		case WIDTH_I64:
			Regs[ OP_A ] = MakeInt( *(int64_t*)Ptr );
			break;
		case WIDTH_U8:
			Regs[ OP_A ] = MakeInt( *(uint8_t*)Ptr );
			break;
		case WIDTH_U16:
			Regs[ OP_A ] = MakeInt( *(uint16_t*)Ptr );
			break;
		case WIDTH_U32:
			Regs[ OP_A ] = MakeInt( *(uint32_t*)Ptr );
			break;
		case WIDTH_U64:
			Regs[ OP_A ] = MakeInt( (Int64)*(uint64_t*)Ptr );
			break;
		case WIDTH_PTR:
			Regs[ OP_A ].Tag = VAL_PTR;
			Regs[ OP_A ].Payload = *(Void**)Ptr;
			break;
		case WIDTH_F32:
			Regs[ OP_A ] = MakeFloat( (double)*(float*)Ptr );
			break;
		case WIDTH_F64:
			Regs[ OP_A ] = MakeFloat( *(double*)Ptr );
			break;
		case WIDTH_OBJ:
			Regs[ OP_A ].Tag = VAL_OBJECT;
			Regs[ OP_A ].Payload = *(Void**)Ptr;
			break;
		case WIDTH_BOOL:
			Regs[ OP_A ] = MakeBool( *(uint8_t*)Ptr != 0 );
			break;
		default:
			SAVE_CURSOR();
			return VM_ERR_BADOP;
		}
	}
	DISPATCH();

LStorePtr:
	{
		Void* Ptr = Regs[ OP_A ].Payload;
		if ( Ptr == NULL )
		{
			SAVE_CURSOR();
			return VM_ERR_NULLPTR;
		}
		FValue Val = Regs[ OP_B ];
		switch ( OP_C )
		{
		case WIDTH_I8:
		case WIDTH_U8:
			*(uint8_t*)Ptr = (uint8_t)AsInt( Val );
			break;
		case WIDTH_BOOL:
			*(uint8_t*)Ptr = (uint8_t)( AsInt( Val ) != 0 );
			break;
		case WIDTH_I16:
		case WIDTH_U16:
			*(uint16_t*)Ptr = (uint16_t)AsInt( Val );
			break;
		case WIDTH_I32:
		case WIDTH_U32:
			*(uint32_t*)Ptr = (uint32_t)AsInt( Val );
			break;
		case WIDTH_I64:
		case WIDTH_U64:
			*(uint64_t*)Ptr = (uint64_t)AsInt( Val );
			break;
		case WIDTH_PTR:
		case WIDTH_OBJ:
			*(Void**)Ptr = Val.Payload;
			break;
		case WIDTH_F32:
			*(float*)Ptr = (float)AsF64( Val );
			break;
		case WIDTH_F64:
			*(double*)Ptr = AsF64( Val );
			break;
		default:
			SAVE_CURSOR();
			return VM_ERR_BADOP;
		}
	}
	DISPATCH();

LPtrAdd:
	Regs[ OP_A ].Tag = VAL_PTR;
	Regs[ OP_A ].Payload = (Void*)( (char*)Regs[ OP_B ].Payload + AsInt( Regs[ OP_C ] ) );
	DISPATCH();

LPtrSub:
	Regs[ OP_A ].Tag = VAL_PTR;
	Regs[ OP_A ].Payload = (Void*)( (char*)Regs[ OP_B ].Payload - AsInt( Regs[ OP_C ] ) );
	DISPATCH();

LAddrLocal:
	Regs[ OP_A ].Tag = VAL_PTR;
	Regs[ OP_A ].Payload = &Regs[ OP_B ].Payload;
	DISPATCH();

LAddrField:
	{
		FHeapObject* Obj = (FHeapObject*)Regs[ OP_B ].Payload;
		if ( Obj == NULL || Regs[ OP_B ].Tag != VAL_OBJECT )
		{
			SAVE_CURSOR();
			return VM_ERR_NILRECEIVER;
		}
		Regs[ OP_A ].Tag = VAL_PTR;
		Regs[ OP_A ].Payload = &Obj->Fields[ OP_C ].Payload;
	}
	DISPATCH();

LAddrGlobal:
	Regs[ OP_A ].Tag = VAL_PTR;
	Regs[ OP_A ].Payload = &Ctx->Globals[ OP_BX ].Payload;
	DISPATCH();
