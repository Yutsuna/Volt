/* ---- cold opcode: hand back to the Crystal driver ------------------- */

LCold:
	Ip -= 1;                       /* rewind to point at the cold instruction */
	SAVE_CURSOR();
	Ctx->ColdOp = (Int32)Op;
	return VM_CALLBACK;

LBadOp:
	Ip -= 1;
	SAVE_CURSOR();
	Ctx->ColdOp = (Int32)Op;
	return VM_ERR_BADOP;
