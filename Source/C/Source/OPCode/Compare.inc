/* ---- comparisons, logical, bitwise, and branch fusion handlers ----------- */

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

LEq:
	{
		FValue V1 = Regs[ OP_B ];
		FValue V2 = Regs[ OP_C ];
		Int32 Result = 0;
		if ( V1.Tag == V2.Tag && V1.Payload == V2.Payload )
		{
			Result = 1;
		}
		else
		{
			Int32 Num1 = ( V1.Tag == VAL_INT || V1.Tag == VAL_FLOAT );
			Int32 Num2 = ( V2.Tag == VAL_INT || V2.Tag == VAL_FLOAT );
			if ( Num1 && Num2 )
			{
				if ( V1.Tag == VAL_INT && V2.Tag == VAL_INT )
				{
					Result = ( AsInt( V1 ) == AsInt( V2 ) );
				}
				else
				{
					double D1 = V1.Tag == VAL_INT ? (double)AsInt( V1 ) : AsF64( V1 );
					double D2 = V2.Tag == VAL_INT ? (double)AsInt( V2 ) : AsF64( V2 );
					Result = ( D1 == D2 );
				}
			}
			else if ( V1.Tag != V2.Tag )
			{
				Result = 0;
			}
			else
			{
				switch ( V1.Tag )
				{
				case VAL_BOOL:
					Result = ( V1.Payload == V2.Payload );
					break;
				case VAL_OBJECT:
					Result = ( V1.Payload == V2.Payload );
					break;
				case VAL_NIL:
					Result = 1;
					break;
				case VAL_STR:
				case VAL_REGEX:
					goto LCold;
				default:
					Result = 0;
					break;
				}
			}
		}
		Regs[ OP_A ] = MakeBool( Result );
	}
	DISPATCH();

LNe:
	{
		FValue V1 = Regs[ OP_B ];
		FValue V2 = Regs[ OP_C ];
		Int32 Result = 0;
		if ( V1.Tag == V2.Tag && V1.Payload == V2.Payload )
		{
			Result = 0;
		}
		else
		{
			Int32 Num1 = ( V1.Tag == VAL_INT || V1.Tag == VAL_FLOAT );
			Int32 Num2 = ( V2.Tag == VAL_INT || V2.Tag == VAL_FLOAT );
			if ( Num1 && Num2 )
			{
				if ( V1.Tag == VAL_INT && V2.Tag == VAL_INT )
				{
					Result = ( AsInt( V1 ) != AsInt( V2 ) );
				}
				else
				{
					double D1 = V1.Tag == VAL_INT ? (double)AsInt( V1 ) : AsF64( V1 );
					double D2 = V2.Tag == VAL_INT ? (double)AsInt( V2 ) : AsF64( V2 );
					Result = ( D1 != D2 );
				}
			}
			else if ( V1.Tag != V2.Tag )
			{
				Result = 1;
			}
			else
			{
				switch ( V1.Tag )
				{
				case VAL_BOOL:
					Result = ( V1.Payload != V2.Payload );
					break;
				case VAL_OBJECT:
					Result = ( V1.Payload != V2.Payload );
					break;
				case VAL_NIL:
					Result = 0;
					break;
				case VAL_STR:
				case VAL_REGEX:
					goto LCold;
				default:
					Result = 1;
					break;
				}
			}
		}
		Regs[ OP_A ] = MakeBool( Result );
	}
	DISPATCH();

LEqInt:
	Regs[ OP_A ] = MakeBool( AsInt( Regs[ OP_B ] ) == AsInt( Regs[ OP_C ] ) );
	DISPATCH();

LNeInt:
	Regs[ OP_A ] = MakeBool( AsInt( Regs[ OP_B ] ) != AsInt( Regs[ OP_C ] ) );
	DISPATCH();

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

LNot:
	Regs[ OP_A ] = MakeBool( !IsTruthy( Regs[ OP_B ] ) );
	DISPATCH();

LConvInt:
	{
		Int64 Val = AsInt( Regs[ OP_A ] );
		Int32 Bits = OP_BX;
		if ( Bits < 64 )
		{
			UInt64 UVal = (UInt64)Val;
			UInt64 Mask = (UInt64)1 << ( Bits - 1 );
			Val = (Int64)( ( UVal & ( ( (UInt64)1 << Bits ) - 1 ) ) ^ Mask ) - (Int64)Mask;
		}
		Regs[ OP_A ] = MakeInt( Val );
	}
	DISPATCH();

LEqIntImm:
	Regs[ OP_A ] = MakeBool( AsInt( Regs[ OP_B ] ) == (Int64)OP_C );
	DISPATCH();

LNeIntImm:
	Regs[ OP_A ] = MakeBool( AsInt( Regs[ OP_B ] ) != (Int64)OP_C );
	DISPATCH();

	/* ---- compare+branch fusion -----------------------------------------
	 * When the comparison is TRUE the branch is not taken: skip the trailing
	 * (untouched) JMP_IF_FALSE. When FALSE, jump to that JMP_IF_FALSE's Bx. */
LBrLtInt:
	if ( AsInt( Regs[ OP_B ] ) <  AsInt( Regs[ OP_C ] ) ) { Ip += 1; } else { Ip = (Int32)( Code[ Ip ] & 0xFFFF ); }
	DISPATCH();

LBrLeInt:
	if ( AsInt( Regs[ OP_B ] ) <= AsInt( Regs[ OP_C ] ) ) { Ip += 1; } else { Ip = (Int32)( Code[ Ip ] & 0xFFFF ); }
	DISPATCH();

LBrGtInt:
	if ( AsInt( Regs[ OP_B ] ) >  AsInt( Regs[ OP_C ] ) ) { Ip += 1; } else { Ip = (Int32)( Code[ Ip ] & 0xFFFF ); }
	DISPATCH();

LBrGeInt:
	if ( AsInt( Regs[ OP_B ] ) >= AsInt( Regs[ OP_C ] ) ) { Ip += 1; } else { Ip = (Int32)( Code[ Ip ] & 0xFFFF ); }
	DISPATCH();

LBrEqInt:
	if ( AsInt( Regs[ OP_B ] ) == AsInt( Regs[ OP_C ] ) ) { Ip += 1; } else { Ip = (Int32)( Code[ Ip ] & 0xFFFF ); }
	DISPATCH();

LBrNeInt:
	if ( AsInt( Regs[ OP_B ] ) != AsInt( Regs[ OP_C ] ) ) { Ip += 1; } else { Ip = (Int32)( Code[ Ip ] & 0xFFFF ); }
	DISPATCH();

LBrLtIntImm:
	if ( AsInt( Regs[ OP_B ] ) <  (Int64)OP_C ) { Ip += 1; } else { Ip = (Int32)( Code[ Ip ] & 0xFFFF ); }
	DISPATCH();

LBrLeIntImm:
	if ( AsInt( Regs[ OP_B ] ) <= (Int64)OP_C ) { Ip += 1; } else { Ip = (Int32)( Code[ Ip ] & 0xFFFF ); }
	DISPATCH();

LBrGtIntImm:
	if ( AsInt( Regs[ OP_B ] ) >  (Int64)OP_C ) { Ip += 1; } else { Ip = (Int32)( Code[ Ip ] & 0xFFFF ); }
	DISPATCH();

LBrGeIntImm:
	if ( AsInt( Regs[ OP_B ] ) >= (Int64)OP_C ) { Ip += 1; } else { Ip = (Int32)( Code[ Ip ] & 0xFFFF ); }
	DISPATCH();

LBrEqIntImm:
	if ( AsInt( Regs[ OP_B ] ) == (Int64)OP_C ) { Ip += 1; } else { Ip = (Int32)( Code[ Ip ] & 0xFFFF ); }
	DISPATCH();

LBrNeIntImm:
	if ( AsInt( Regs[ OP_B ] ) != (Int64)OP_C ) { Ip += 1; } else { Ip = (Int32)( Code[ Ip ] & 0xFFFF ); }
	DISPATCH();
