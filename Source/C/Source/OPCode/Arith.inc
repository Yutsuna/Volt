/* ---- integer & float arithmetic handlers --------------------------------- */

LAddInt:
	{
		Int64 R;
		if ( __builtin_add_overflow( AsInt( Regs[ OP_B ] ), AsInt( Regs[ OP_C ] ), &R ) )
		{
			SAVE_CURSOR();
			return VM_ERR_OVERFLOW;
		}
		Regs[ OP_A ] = MakeInt( R );
	}
	DISPATCH();

LSubInt:
	{
		Int64 R;
		if ( __builtin_sub_overflow( AsInt( Regs[ OP_B ] ), AsInt( Regs[ OP_C ] ), &R ) )
		{
			SAVE_CURSOR();
			return VM_ERR_OVERFLOW;
		}
		Regs[ OP_A ] = MakeInt( R );
	}
	DISPATCH();

LMulInt:
	{
		Int64 R;
		if ( __builtin_mul_overflow( AsInt( Regs[ OP_B ] ), AsInt( Regs[ OP_C ] ), &R ) )
		{
			SAVE_CURSOR();
			return VM_ERR_OVERFLOW;
		}
		Regs[ OP_A ] = MakeInt( R );
	}
	DISPATCH();

LDivInt:
LIDivInt:
	{
		Int64 Divisor = AsInt( Regs[ OP_C ] );
		if ( Divisor == 0 )
		{
			SAVE_CURSOR();
			return VM_ERR_DIVZERO;
		}
		Regs[ OP_A ] = MakeInt( FloorDiv( AsInt( Regs[ OP_B ] ), Divisor ) );
	}
	DISPATCH();

LModInt:
	{
		Int64 Divisor = AsInt( Regs[ OP_C ] );
		if ( Divisor == 0 )
		{
			SAVE_CURSOR();
			return VM_ERR_DIVZERO;
		}
		Regs[ OP_A ] = MakeInt( FloorMod( AsInt( Regs[ OP_B ] ), Divisor ) );
	}
	DISPATCH();

LNegInt:
	{
		Int64 R;
		if ( __builtin_sub_overflow( (Int64)0, AsInt( Regs[ OP_B ] ), &R ) )
		{
			SAVE_CURSOR();
			return VM_ERR_OVERFLOW;
		}
		Regs[ OP_A ] = MakeInt( R );
	}
	DISPATCH();

LAddF64:
	Regs[ OP_A ] = MakeFloat( AsF64( Regs[ OP_B ] ) + AsF64( Regs[ OP_C ] ) );
	DISPATCH();

LSubF64:
	Regs[ OP_A ] = MakeFloat( AsF64( Regs[ OP_B ] ) - AsF64( Regs[ OP_C ] ) );
	DISPATCH();

LMulF64:
	Regs[ OP_A ] = MakeFloat( AsF64( Regs[ OP_B ] ) * AsF64( Regs[ OP_C ] ) );
	DISPATCH();

LDivF64:
	Regs[ OP_A ] = MakeFloat( AsF64( Regs[ OP_B ] ) / AsF64( Regs[ OP_C ] ) );
	DISPATCH();

LNegF64:
	Regs[ OP_A ] = MakeFloat( -AsF64( Regs[ OP_B ] ) );
	DISPATCH();

LAddIntImm:
	{
		Int64 R;
		if ( __builtin_add_overflow( AsInt( Regs[ OP_B ] ), (Int64)OP_C, &R ) )
		{
			SAVE_CURSOR();
			return VM_ERR_OVERFLOW;
		}
		Regs[ OP_A ] = MakeInt( R );
	}
	DISPATCH();

LSubIntImm:
	{
		Int64 R;
		if ( __builtin_sub_overflow( AsInt( Regs[ OP_B ] ), (Int64)OP_C, &R ) )
		{
			SAVE_CURSOR();
			return VM_ERR_OVERFLOW;
		}
		Regs[ OP_A ] = MakeInt( R );
	}
	DISPATCH();
