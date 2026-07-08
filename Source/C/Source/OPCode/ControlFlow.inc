/* ---- control flow jump handlers ------------------------------------------ */

LJmp:
	Ip = OP_BX;
	DISPATCH();

LJmpIfFalse:
	if ( !IsTruthy( Regs[ OP_A ] ) )
	{
		Ip = OP_BX;
	}
	DISPATCH();
