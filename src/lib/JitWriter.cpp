#include "stdafx.h"
#include "wasm_types.h"
#include "Exceptions.h"
#include "safe_access.h"
#include "JitWriter.h"
#include "WasmContext.h"
#include "ExecutionControlBlock.h"
#include "numeric_cast.h"

extern "C" void WasmToC();
extern "C" void CallIndirectShim();
extern "C" void BranchTable();
extern "C" void U64ToF32();
extern "C" void U64ToF64();
extern "C" void GrowMemoryOp();
extern "C" void F32ToU64Trunc();
extern "C" void F64ToU64Trunc();

JitWriter::JitWriter(WasmContext *pctxt, size_t cfn, size_t cglbls)
	: m_pctxt(pctxt), m_pexecPlane(nullptr), m_cfn(cfn)
{
	const size_t cbExec = 0x40000000; 	// 1Gb
#ifdef _DEBUG
	const void *pvBase = (const void*)0xB00000000;	// In debug its nice to have a constant base, in ship we want aslr
#else
	const void *pvBase = nullptr;
#endif
	m_spapbExecPlane = layer::ReservePages(pvBase, cbExec);
	ProtectRange(*m_spapbExecPlane, 0, cbExec, layer::PAGE_PROTECTION::ReadWrite);
	m_pexecPlane = (uint8_t*)m_spapbExecPlane->PvBaseAddr();
	m_pexecPlaneCur = m_pexecPlane;
	m_pexecPlaneMax = m_pexecPlaneCur + m_spapbExecPlane->Cb();


	uint8_t *pvZeroStart = m_pexecPlaneCur;
	m_pexecPlaneCur += sizeof(void*) * cfn;	// allocate the function table, ensuring its within 32-bits of all our code
	
	m_pfnCallIndirectShim = (void**)m_pexecPlaneCur;
	m_pfnBranchTable = ((void**)m_pexecPlaneCur) + 1;
	m_pfnU64ToF32 = ((void**)m_pexecPlaneCur) + 2;
	m_pfnU64ToF64 = ((void**)m_pexecPlaneCur) + 3;
	m_pfnGrowMemoryOp = ((void**)m_pexecPlaneCur) + 4;
	m_pfnF32ToU64Trunc = ((void**)m_pexecPlaneCur) + 5;
	m_pfnF64ToU64Trunc = ((void**)m_pexecPlaneCur) + 6;
	m_pexecPlaneCur += sizeof(*m_pfnCallIndirectShim) * 7;

	m_pexecPlaneCur += (4096 - reinterpret_cast<uint64_t>(m_pexecPlaneCur)) % 4096;
	m_pGlobalsStart = (uint64_t*)m_pexecPlaneCur;
	m_pexecPlaneCur += (sizeof(uint64_t) * cglbls);
	m_pexecPlaneCur += (4096 - reinterpret_cast<uint64_t>(m_pexecPlaneCur)) % 4096;
	m_pcodeStart = m_pexecPlaneCur;
	memset(pvZeroStart, 0, m_pexecPlaneCur - pvZeroStart);	// these areas should be initialized to zero

	for (size_t iimportfn = 0; iimportfn < m_pctxt->m_vecimports.size(); ++iimportfn)
		reinterpret_cast<void**>(m_pexecPlane)[iimportfn] = (void*)WasmToC;
	*m_pfnCallIndirectShim = (void*)CallIndirectShim;
	*m_pfnBranchTable = (void*)BranchTable;
	*m_pfnU64ToF32 = (void*)U64ToF32;
	*m_pfnU64ToF64 = (void*)U64ToF64;
	*m_pfnGrowMemoryOp = (void*)GrowMemoryOp;
	*m_pfnF32ToU64Trunc = (void*)F32ToU64Trunc;
	*m_pfnF64ToU64Trunc = (void*)F64ToU64Trunc;

	for (size_t iglbl = 0; iglbl < cglbls; ++iglbl)
	{
		m_pGlobalsStart[iglbl] = pctxt->m_vecglbls[iglbl].val;
	}
}

JitWriter::~JitWriter()
{
}

void JitWriter::SafePushCode(const void *pv, size_t cb)
{
	if (m_pexecPlaneCur + cb > m_pexecPlaneMax)
		throw RuntimeException("No room to compile function");
	memcpy(m_pexecPlaneCur, pv, cb);
	m_pexecPlaneCur += cb;
}

void JitWriter::_PushExpandStack()
{
	// RAX -> stack
	// mov [rdi], rax		{ 0x48, 0x89, 0x07 }
	// add rdi, 8			{ 0x48, 0x83, 0xC7, 0x08 }
	static const uint8_t rgcode[] = { 0x48, 0x89, 0x07, 0x48, 0x83, 0xC7, 0x08 };
	SafePushCode(rgcode, _countof(rgcode));
}

// NOTE: Must not affect flags
void JitWriter::_PopContractStack()
{
	// mov rax, [rdi - 8]	{ 0x48, 0x8b, 0x47, 0xf8 }
	// lea rdi, [rdi - 8]	{ 0x48, 0x8D, 0x7F, 0xF8 }
	static const uint8_t rgcode[] = { 0x48, 0x8B, 0x47, 0xF8, 0x48, 0x8D, 0x7F, 0xF8 };
	SafePushCode(rgcode, _countof(rgcode));
}

void JitWriter::_PopSecondParam(bool fSwapParams)
{
	if (fSwapParams)
	{
		// mov rcx, rax
		// mov rax, [rdi - 8]
		// sub rdi, 8
		static const uint8_t rgcode[] = { 0x48, 0x89, 0xC1, 0x48, 0x8B, 0x47, 0xF8, 0x48, 0x83, 0xEF, 0x08 };
		SafePushCode(rgcode, _countof(rgcode));
	}
	else
	{
		// mov rcx, [rdi - 8]
		// sub rdi, 8
		static const uint8_t rgcode[] = { 0x48, 0x8B, 0x4F, 0xF8, 0x48, 0x83, 0xEF, 0x08 };
		SafePushCode(rgcode, _countof(rgcode));
	}
}


void JitWriter::LoadMem(uint32_t offset, bool f64Dst /* else 32 */, uint32_t cbSrc, bool fSignExtend)
{
	// add eax, offset	; note this implicitly ands with 0xffffffff ensuring that when referenced as rax it will always be a positive number between 0 and 2^32 - 1
	static const uint8_t rgcodeAdd[] = { 0x05 };
	SafePushCode(rgcodeAdd);
	SafePushCode(offset);

	const char *szCode = nullptr;
	if (fSignExtend)
	{
		if (f64Dst)
		{
			switch (cbSrc)
			{
			default:
				Verify(false);
				break;

			case 1:
				// movsx rax, byte ptr [rsi+rax]
				szCode = "\x48\x0F\xBE\x04\x06";
				break;

			case 2:
				// movsx rax, word ptr [rsi+rax]
				szCode = "\x48\x0F\xBF\x04\x06";
				break;

			case 4:
				// movsx rax, dword ptr [rsi+rax]
				szCode = "\x48\x63\x04\x06";
				break;
			}
		}
		else
		{
			switch (cbSrc)
			{
			default:
				Verify(false);
				break;

			case 1:
				// movsx eax, byte ptr [rsi + rax]
				szCode = "\x0F\xBE\x04\x06";
				break;

			case 2:
				// movsx eax, word ptr [rsi + rax]
				szCode = "\x0F\xBF\x04\x06";
			}
		}
	}
	else
	{
		switch (cbSrc)
		{
		case 1:
			// movzx eax, byte ptr [rsi+rax]
			szCode = "\x0F\xB6\x04\x06";
			break;

		case 2:
			// movzx eax, word ptr [rsi + rax]
			szCode = "\x0F\xB7\x04\x06";
			break;

		case 4:
			// mov eax, dword ptr [rsi + rax]
			szCode = "\x8B\x04\x06";
			break;

		case 8:
			Verify(f64Dst);
			// mov rax, qword ptr [rsi + rax]
			szCode = "\x48\x8B\x04\x06";
			break;

		default:
			Verify(false);
		}
	}
	Verify(szCode != nullptr);
	SafePushCode(szCode, strlen(szCode));
}
void JitWriter::StoreMem(uint32_t offset, uint32_t cbDst)
{
	_PopSecondParam();
	// add ecx, offset	; note this implicitly ands with 0xffffffff ensuring that when referenced as rax it will always be a positive number between 0 and 2^32 - 1
	static const uint8_t rgcodeAdd[] = { 0x81, 0xC1 };
	SafePushCode(rgcodeAdd);
	SafePushCode(offset);

	const char *szCode = nullptr;
	switch (cbDst)
	{
	default:
		Verify(false);
		break;

	case 1:
		// mov [rsi + rcx], al
		szCode = "\x88\x04\x0E";
		break;

	case 2:
		// mov [rsi + rcx], ax
		szCode = "\x66\x89\x04\x0E";
		break;

	case 4:
		// mov [rsi + rcx], eax
		szCode = "\x89\x04\x0E";
		break;

	case 8:
		// mov [rsi + rcx], rax
		szCode = "\x48\x89\x04\x0E";
		break;
	}
	Verify(szCode != nullptr);
	SafePushCode(szCode, strlen(szCode));
}

void JitWriter::Sub32()
{
	_PopSecondParam(true);
	// sub eax, ecx
	static const uint8_t rgcode[] = { 0x29, 0xC8 };
	SafePushCode(rgcode, _countof(rgcode));
}

void JitWriter::Add32()
{
	_PopSecondParam();
	// add eax, ecx
	static const uint8_t rgcode[] = { 0x01, 0xC8 };
	SafePushCode(rgcode, _countof(rgcode));
}

void JitWriter::Add64()
{
	_PopSecondParam();
	// add rax, rcx
	static const uint8_t rgcode[] = { 0x48, 0x01, 0xC8 };
	SafePushCode(rgcode);
}

void JitWriter::Sub64()
{
	_PopSecondParam(true);
	// sub rax, rcx
	static const uint8_t rgcode[] = { 0x48, 0x29, 0xC8 };
	SafePushCode(rgcode);
}

void JitWriter::Popcnt32()
{
	// mov [rdi], eax		; popcnt wants a memory operand - gross
	// popcnt eax, [rdi]
	const uint8_t rgcode[] = { 0x89, 0x07, 0xF3, 0x0F, 0xB8, 0x07 };
	SafePushCode(rgcode, _countof(rgcode));
}

void JitWriter::Popcnt64()
{
	// mov[rdi], rax
	// popcnt rax, qword ptr[rdi]
	const uint8_t rgcode[] = { 0x48, 0x89, 0x07, 0xF3, 0x48, 0x0F, 0xB8, 0x07 };
	SafePushCode(rgcode);
}

void JitWriter::PushC32(uint32_t c)
{
	_PushExpandStack();
	if (c == 0)	// micro optimize zeroing
	{
		// xor eax, eax
		static const uint8_t rgcode[] = { 0x31, 0xC0 };
		SafePushCode(rgcode, _countof(rgcode));
	}
	else
	{
		// mov eax, c
		static const uint8_t rgcode[] = { 0xB8 };
		SafePushCode(rgcode, sizeof(uint8_t));
		SafePushCode(&c, sizeof(c));
	}
}

void JitWriter::PushC64(uint64_t c)
{
	_PushExpandStack();
	if (c == 0) // micro optimize zeroing
	{
		// xor rax, rax
		static const uint8_t rgcode[] = { 0x48, 0x31, 0xC0 };
		SafePushCode(rgcode, _countof(rgcode));
	}
	else
	{
		// mov rax, c
		static const uint8_t rgcode[] = { 0x48, 0xB8 };
		SafePushCode(rgcode, _countof(rgcode));
		SafePushCode(&c, sizeof(c));
	}
}

void JitWriter::PushF32(float val)
{
	int32_t ival = *reinterpret_cast<int32_t*>(&val);
	PushC32(ival);
}

void JitWriter::PushF64(double val)
{
	int64_t ival = *reinterpret_cast<int64_t*>(&val);
	PushC64(ival);
}

void JitWriter::Ud2()
{
	// NYI: ud2
	static const uint8_t rgcode[] = { 0x0F, 0x0B };
	SafePushCode(rgcode, _countof(rgcode));
}

void JitWriter::SetLocal(uint32_t idx, bool fPop)
{
	// mov [rbx+idx], rax
	static const uint8_t rgcode[] = { 0x48, 0x89, 0x83 };
	SafePushCode(rgcode, _countof(rgcode));
	idx *= sizeof(uint64_t);
	SafePushCode(&idx, sizeof(idx));
	if (fPop)
		_PopContractStack();
}

void JitWriter::GetLocal(uint32_t idx)
{
	_PushExpandStack();
	// mov rax, [rbx+idx]
	static const uint8_t rgcode[] = { 0x48, 0x8B, 0x83 };
	idx *= sizeof(uint64_t);
	SafePushCode(rgcode, _countof(rgcode));
	SafePushCode(&idx, sizeof(idx));
}

void JitWriter::EnterBlock()
{
	_PushExpandStack();	// backup rax
	// push rdi
	static const uint8_t rgcode[] = { 0x57 };
	SafePushCode(rgcode, _countof(rgcode));
}

int32_t *JitWriter::EnterIF()
{
	// sub rdi, 8
	// test rax, rax
	// mov rax, [rdi]
	static const uint8_t rgcodeTest[] = { 0x48, 0x83, 0xEF, 0x08, 0x48, 0x85, 0xC0, 0x48, 0x8B, 0x07 };
	SafePushCode(rgcodeTest);
	// jz rel32
	static const uint8_t rgcodeJz[] = {0x0F, 0x84};
	SafePushCode(rgcodeJz);
	int32_t *prel32Ret = (int32_t*)m_pexecPlaneCur;
	SafePushCode(int32_t(0));	// placeholder

	_PushExpandStack();	// backup rax

	// push rdi
	static const uint8_t rgcode[] = { 0x57 };
	SafePushCode(rgcode);

	return prel32Ret;
}

void JitWriter::LeaveBlock(bool fHasReturn)
{
	// pop rdi
	static const uint8_t rgcode[] = { 0x5F };
	SafePushCode(rgcode, _countof(rgcode));
	if (!fHasReturn)
		_PopContractStack();
}

void JitWriter::Eqz32()
{
	//	test eax, eax		{ 0x85, 0xC0 }
	//	mov eax, 1			{ 0xb8, 0x01, 0x00, 0x00, 0x00}
	//  jz  Lz				{ 0x74, 0x02 }
	//	xor eax, eax		{ 0x31, 0xC0 }
	// Lz:
	static const uint8_t rgcode[] = { 0x85, 0xC0, 0xB8, 0x01, 0x00, 0x00, 0x00, 0x74, 0x02, 0x31, 0xC0 };
	SafePushCode(rgcode, _countof(rgcode));
}

void JitWriter::Eqz64()
{
	//	test rax, rax		{ 0x48, 0x85, 0xC0 }
	//	mov eax, 1			{ 0xb8, 0x01, 0x00, 0x00, 0x00}
	//  jz  Lz				{ 0x74, 0x02 }
	//	xor eax, eax		{ 0x31, 0xC0 }
	// Lz:
	static const uint8_t rgcode[] = { 0x48, 0x85, 0xC0, 0xB8, 0x01, 0x00, 0x00, 0x00, 0x74, 0x02, 0x31, 0xC0 };
	SafePushCode(rgcode, _countof(rgcode));
}

void JitWriter::Compare(CompareType type, bool fSigned, bool f64)
{
	_PopSecondParam();
	//		cmp rcx, rax		{ 0x48, 0x39, 0xc1 }			// yes the order is reversed... this is by the spec
	//		mov eax, 1			{ b8 01 00 00 00}
	//		jcc LTrue			{ XX, 0x02 }	2-byte forward rel jump
	//		xor eax, eax		{ 31 c0 }
	// LTrue:
	static const uint8_t rgcodeCmp64[] = { 0x48, 0x39, 0xc1 };
	static const uint8_t rgcodeCmp32[] = { 0x39, 0xC1 };
	static const uint8_t rgcodePre[] = { 0xb8, 0x01, 0x00, 0x00, 0x00 };
	static const uint8_t rgcodePost[] = { 0x02, 0x31, 0xc0 };

	if (f64)
		SafePushCode(rgcodeCmp64, _countof(rgcodeCmp64));
	else
		SafePushCode(rgcodeCmp32, _countof(rgcodeCmp32));

	SafePushCode(rgcodePre, _countof(rgcodePre));
	uint8_t cmpOp = 0xCC;	// int3 BUG
	switch (type)
	{
	case CompareType::LessThan:
		if (fSigned)
			cmpOp = 0x7C;	// JL  rel8
		else
			cmpOp = 0x72;	// JB  rel8
		break;

	case CompareType::LessThanEqual:
		if (fSigned)
			cmpOp = 0x7E;	// JLE rel8
		else
			cmpOp = 0x76;	// JBE rel8
		break;

	case CompareType::Equal:
		cmpOp = 0x74;		// JE rel8
		break;

	case CompareType::NotEqual:
		cmpOp = 0x75;		// JNE rel8
		break;

	case CompareType::GreaterThanEqual:
		if (fSigned)
			cmpOp = 0x7D;	// JGE rel8
		else
			cmpOp = 0x73;	// JAE rel8
		break;

	case CompareType::GreaterThan:
		if (fSigned)
			cmpOp = 0x7F;	// JG rel8
		else
			cmpOp = 0x77;	// JA rel8
		break;

	default:
		Verify(false);
	}
	SafePushCode(&cmpOp, sizeof(cmpOp));
	SafePushCode(rgcodePost, _countof(rgcodePost));
}

void JitWriter::CallAsmOp(void **pfn)
{
	static const uint8_t rgcodeCallIndirect[] = { uint8_t(0xFF), uint8_t(0x15) };
	SafePushCode(rgcodeCallIndirect);
	ptrdiff_t diffFn = reinterpret_cast<ptrdiff_t>(pfn) - (reinterpret_cast<ptrdiff_t>(m_pexecPlaneCur) + 4);
	Verify(static_cast<int32_t>(diffFn) == diffFn);
	SafePushCode(int32_t(diffFn));
}

void JitWriter::Convert(value_type typeDst, value_type typeSrc, bool fSigned)
{
	const char *szConv = nullptr;
	switch (typeDst)
	{
	case value_type::i32:
		switch (typeSrc)
		{
		case value_type::f32:
			if (fSigned)
			{
				// movd xmm0, eax
				// CVTTSS2SI eax, xmm0
				szConv = "\x66\x0F\x6E\xC0\xF3\x0F\x2C\xC0";
			}
			else
			{
				// movd xmm0, eax
				// CVTTSS2SI rax, xmm0
				// mov eax, eax
				szConv = "\x66\x0F\x6E\xC0\xF3\x48\x0F\x2C\xC0\x89\xC0";
			}
			break;

		case value_type::f64:
			if (fSigned)
			{
				// movq xmm0, rax
				// CVTTSD2SI eax, xmm0
				szConv = "\x66\x48\x0F\x6E\xC0\xF2\x0F\x2C\xC0";
			}
			else
			{
				// movq xmm0, rax
				// CVTTSD2SI rax, xmm0
				// mov eax, eax
				szConv = "\x66\x48\x0F\x6E\xC0\xF2\x48\x0F\x2C\xC0\x89\xC0";
			}
			break;
		}
		break;

	case value_type::i64:
		switch (typeSrc)
		{
		case value_type::f32:
			if (fSigned)
			{
				// movd xmm0, eax
				// CVTTSS2SI rax, xmm0
				szConv = "\x66\x0F\x6E\xC0\xF3\x48\x0F\x2C\xC0";
			}
			else
			{
				CallAsmOp(m_pfnF32ToU64Trunc);
				return;
			}
			break;
		
		case value_type::f64:
			if (fSigned)
			{
				// movq xmm0, rax
				// cvttsd2si rax, xmm0
				szConv = "\x66\x48\x0F\x6E\xC0\xF2\x48\x0F\x2C\xC0";
			}
			else
			{
				CallAsmOp(m_pfnF64ToU64Trunc);
				return;
			}
		}
		break;

	case value_type::f32:
		switch (typeSrc)
		{
		case value_type::i32:
			if (fSigned)
			{
				// cvtsi2ss xmm0, eax
				// movd eax, xmm0
				szConv = "\xF3\x0F\x2A\xC0\x66\x0F\x7E\xC0";
			}
			else
			{
				// cvtsi2ss xmm0, rax
				// movd eax, xmm0
				szConv = "\xF3\x48\x0F\x2A\xC0\x66\x0F\x7E\xC0";
			}
			break;

		case value_type::i64:
			if (fSigned)
			{
				// cvtsi2ss xmm0, rax
				// movd eax, xmm0
				szConv = "\xF3\x48\x0F\x2A\xC0\x66\x0F\x7E\xC0";
			}
			else
			{
				CallAsmOp(m_pfnU64ToF32);
				return;
			}
			break;

		case value_type::f64:
			// movq xmm0, rax
			// cvtsd2ss xmm0, xmm0
			// movd eax, xmm0
			szConv = "\x66\x48\x0F\x6E\xC0\xF2\x0F\x5A\xC0\x66\x0F\x7E\xC0";
			break;
		}
		break;

	case value_type::f64:
		switch (typeSrc)
		{
		case value_type::i32:
			if (!fSigned)
			{
				// cvtsi2sd xmm0, rax
				// movq rax, xmm0
				szConv = "\xF2\x48\x0F\x2A\xC0\x66\x48\x0F\x7E\xC0";
			}
			else
			{
				// cvtsi2sd xmm0, eax
				// movq rax, xmm0
				szConv = "\xF2\x0F\x2A\xC0\x66\x48\x0F\x7E\xC0";
			}
			break;
		case value_type::i64:
			if (!fSigned)
			{
				// call [m_pfnU64ToF64]
				CallAsmOp(m_pfnU64ToF64);
				return;
			}
			else
			{
				// cvtsi2sd xmm0, rax
				// movq rax, xmm0
				szConv = "\xF2\x48\x0F\x2A\xC0\x66\x48\x0F\x7E\xC0";
			}
			break;
		case value_type::f32:
			// movd xmm0, eax
			// cvtss2sd xmm0, xmm0
			// movq rax, xmm0
			szConv = "\x66\x0F\x6E\xC0\xF3\x0F\x5A\xC0\x66\x48\x0F\x7E\xC0";
			break;
		}
		break;
	}
	Verify(szConv != nullptr);
	SafePushCode(szConv, strlen(szConv));
}

void JitWriter::FloatCompare(CompareType type)
{
	// Note: Because cmpss only does less than compares we may swap the operands
	bool fSwap = false;
	switch (type)
	{
	case CompareType::GreaterThan:
	case CompareType::GreaterThanEqual:
		fSwap = true;
	}

	// sub rdi, 8 (for the variable we're about to pop)
	static const uint8_t rgcodeFixRdi[] = { 0x48, 0x83, 0xEF, 0x08 };
	SafePushCode(rgcodeFixRdi);

	if (fSwap)
	{
		// movd xmm0, eax
		// movd xmm1, [rdi]
		static const uint8_t rgcodeMov[] = { 0x66, 0x0F, 0x6E, 0xC0, 0x66, 0x0F, 0x6E, 0x0F };
		SafePushCode(rgcodeMov);
	}
	else
	{
		// movd xmm1, eax		; second operand
		// movd xmm0, [rdi]		; first operand
		static const uint8_t rgcodeMov[] = { 0x66, 0x0F, 0x6E, 0xC8, 0x66, 0x0F, 0x6E, 0x07 };
		SafePushCode(rgcodeMov);
	}

	uint8_t cmpssImm;
	switch (type)
	{
	default:
		Verify(false);

	case CompareType::LessThan:
	case CompareType::GreaterThan:
		cmpssImm = 1;
		break;

	case CompareType::LessThanEqual:
	case CompareType::GreaterThanEqual:
		cmpssImm = 2;
		break;

	case CompareType::Equal:
		cmpssImm = 0;
		break;

	case CompareType::NotEqual:
		cmpssImm = 4;
		break;
	}
	// cmpss xmm0, xmm1, imm8
	static const uint8_t rgcodeCmpss[] = { 0xF3, 0x0F, 0xC2, 0xC1 };
	SafePushCode(rgcodeCmpss);
	SafePushCode(uint8_t(cmpssImm));

	// movd eax, xmm0
	// and eax, 1
	static const uint8_t rgcodeReduceFlag[] = { 0x66, 0x0F, 0x7E, 0xC0, 0x83, 0xE0, 0x01 };
	SafePushCode(rgcodeReduceFlag);
}

void JitWriter::DoubleCompare(CompareType type)
{
	// Note: Because cmpsd only does less than compares we may swap the operands
	bool fSwap = false;
	switch (type)
	{
	case CompareType::GreaterThan:
	case CompareType::GreaterThanEqual:
		fSwap = true;
	}

	if (fSwap)
	{
		// movq xmm0, rax
		// movq xmm1, [rdi]
		static const uint8_t rgcodeMov[] = { 0x66, 0x48, 0x0F, 0x6E, 0xC0, 0xF3, 0x0F, 0x7E, 0x0F };
		SafePushCode(rgcodeMov);
	}
	else
	{
		// movq xmm1, rax		; second operand
		// movq xmm0, [rdi]		; first operand
		static const uint8_t rgcodeMov[] = { 0x66, 0x48, 0x0F, 0x6E, 0xC8, 0xF3, 0x0F, 0x7E, 0x07 };
		SafePushCode(rgcodeMov);
	}

	uint8_t cmpsdImm;
	switch (type)
	{
	default:
		Verify(false);

	case CompareType::LessThan:
	case CompareType::GreaterThan:
		cmpsdImm = 1;
		break;

	case CompareType::LessThanEqual:
	case CompareType::GreaterThanEqual:
		cmpsdImm = 2;
		break;

	case CompareType::Equal:
		cmpsdImm = 0;
		break;

	case CompareType::NotEqual:
		cmpsdImm = 4;
		break;
	}
	// sub rdi, 8			; fixup stack for the op we just loaded
	// cmpsd xmm0, xmm1, imm8
	static const uint8_t rgcodeCmpss[] = { 0x48, 0x83, 0xEF, 0x08, 0xF2, 0x0F, 0xC2, 0xC1 };
	SafePushCode(rgcodeCmpss);
	SafePushCode(uint8_t(cmpsdImm));

	// movd eax, xmm0
	// and eax, 1
	static const uint8_t rgcodeReduceFlag[] = { 0x66, 0x0F, 0x7E, 0xC0, 0x83, 0xE0, 0x01 };
	SafePushCode(rgcodeReduceFlag);
}

void JitWriter::FloatNeg(bool f64)
{
	// the sign bit is the most significant in IEEE float format
	if (f64)
	{
		// mov rcx, 0x8000000000000000
		// xor rax, rcx
		static const uint8_t rgcode[] = { 0x48, 0xB9, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x48, 0x31, 0xC8 };
		SafePushCode(rgcode);
	}
	else
	{
		// mov ecx, 0x80000000
		// xor eax, ecx
		static const uint8_t rgcode[] = { 0xB9, 0x00, 0x00, 0x00, 0x80, 0x31, 0xC8 };
		SafePushCode(rgcode);
	}
}

int32_t *JitWriter::JumpNIf(void *pvJmp)
{
	int32_t offset = -6;
	// test rax, rax { 0x48, 0x85, 0xC0 }
	static const uint8_t rgcodeTest[] = { 0x48, 0x85, 0xC0 };
	SafePushCode(rgcodeTest, _countof(rgcodeTest));
	_PopContractStack();		// does not affect flags

	if (pvJmp != nullptr)
	{
		int64_t offset64 = reinterpret_cast<uint8_t*>(pvJmp) - (reinterpret_cast<uint8_t*>(m_pexecPlaneCur) + 6);
		offset = static_cast<int32_t>(offset64);
		Verify(offset64 == offset);
	}
	// JZ rel32	{ 0x0F, 0x84, REL32 }
	static const uint8_t rgcodeJRel[] = { 0x0F, 0x84 };
	SafePushCode(rgcodeJRel, _countof(rgcodeJRel));
	int32_t *poffsetRet = (int32_t*)m_pexecPlaneCur;
	SafePushCode(&offset, sizeof(offset));
	return poffsetRet;
}

int32_t *JitWriter::Jump(void *pvJmp)
{
	int32_t offset = -5;
	
	if (pvJmp != nullptr)
	{
		int64_t offset64 = reinterpret_cast<uint8_t*>(pvJmp) - (reinterpret_cast<uint8_t*>(m_pexecPlaneCur) + 5);
		offset = static_cast<int32_t>(offset64);
		Verify(offset == offset64);
	}
	// JMP rel32	{ 0xE9, REL32 }
	static const uint8_t rgcodeJRel[] = { 0xE9 };
	SafePushCode(rgcodeJRel, _countof(rgcodeJRel));
	int32_t *poffsetRet = (int32_t*)m_pexecPlaneCur;
	SafePushCode(&offset, sizeof(offset));
	return poffsetRet;
}

void JitWriter::CallIfn(uint32_t ifn, uint32_t clocalsCaller, uint32_t cargsCallee, bool fReturnValue, bool fIndirect)
{
	// Stage 1: Push arguments
	//	add	 rbx, (clocalsCaller * sizeof(uint64_t))
	static const uint8_t rgcodeAllocLocals[] = { 0x48, 0x81, 0xC3 };
	SafePushCode(rgcodeAllocLocals, _countof(rgcodeAllocLocals));
	uint32_t cbLocals = (clocalsCaller * sizeof(uint64_t));
	SafePushCode(&cbLocals, sizeof(cbLocals));

	if (fIndirect)
	{
		// the top of stack is the function index
		// back it up into rcx (mov rcx, rax)
		static const uint8_t rgT[] = { 0x48, 0x89, 0xC1 };
		SafePushCode(rgT);
		_PopContractStack();
	}

	// pop arguments into the newly allocated local variable region
	while (cargsCallee > 0)
	{
		SetLocal(cargsCallee - 1, true);
		--cargsCallee;
	}
	
	_PushExpandStack();	// put the top of the stack into RAM so we can recover it
	//  push rdi		; backup the operand stack
	static const uint8_t rgcode[] = { 0x57 };
	SafePushCode(rgcode, _countof(rgcode));

	if (fIndirect)
	{
		// ifn is the type in this case
		// mov eax, ifn
		SafePushCode(uint8_t(0xB8));
		SafePushCode(uint32_t(ifn));

		// call [m_pfnCallIndirectShim]
		static const uint8_t rgcodeCallIndirect[] = { uint8_t(0xFF), uint8_t(0x15) };
		SafePushCode(rgcodeCallIndirect);
		ptrdiff_t diffFn = reinterpret_cast<ptrdiff_t>(m_pfnCallIndirectShim) - reinterpret_cast<ptrdiff_t>(m_pexecPlaneCur + 4);
		Verify(static_cast<int32_t>(diffFn) == diffFn);
		SafePushCode(int32_t(diffFn));
	}
	else
	{
		if (ifn < m_pctxt->m_vecimports.size())
		{
			// mov ecx ifn ; so we know the function
			static const uint8_t rgcodeFnNum[] = { 0xB9 };
			SafePushCode(rgcodeFnNum);
			SafePushCode(ifn);
		}

		// Stage 2, call the actual function
		// call [rip - PfnVector]
		static const uint8_t rgcodeCall[] = { 0xFF, 0x15 };
		int32_t offset = RelAddrPfnVector(ifn, 6);
		SafePushCode(rgcodeCall, _countof(rgcodeCall));
		SafePushCode(&offset, sizeof(offset));
	}

	// Stage 3, on return cleanup the stack
	//	pop rdi
	//	sub rbx, (clocalsCaller * sizeof(uint64_t))
	static const uint8_t rgcodeCleanup[] = { 0x5F, 0x48, 0x81, 0xEB };
	SafePushCode(rgcodeCleanup, _countof(rgcodeCleanup));
	SafePushCode(&cbLocals, sizeof(cbLocals));

	// Stage 4, if there was no return value then place our last operand back in rax
	if (!fReturnValue)
	{
		_PopContractStack();
	}
}

void JitWriter::FnPrologue(uint32_t clocals, uint32_t cargs)
{
	// memset local variables to zero
	// ZeroMemory(rbx + (cargs * sizeof(uint64_t)), (clocals - cargs) * sizeof(uint64_t))
	uint32_t clocalsNoArgs = clocals - cargs;

	if (clocalsNoArgs > 0)
	{
		// xor eax, eax						; rax is clobbered so no need to save it
		// mov rdx, rdi						; backup rdi
		// lea rdi, [rbx + localOffset]
		// mov rcx, (clocals - cargs)
		// rep stos qword ptr [rdi]
		// mov rdi, rdx

		static const uint8_t rgcodeXorEax[] = { 0x31, 0xC0, 0x48, 0x89, 0xFA };	// xor eax, eax  mov rdx, rdi
		SafePushCode(rgcodeXorEax);

		static const uint8_t rgcodeLeaRdx[] = { 0x48, 0x8D, 0xBB };	// lea rdi, [rbx + XYZ]
		int32_t cbOffset = (cargs * sizeof(uint64_t));
		SafePushCode(rgcodeLeaRdx, _countof(rgcodeLeaRdx));
		SafePushCode(&cbOffset, sizeof(cbOffset));

		static const uint8_t rgcodeMovRcx[] = { 0x48, 0xC7, 0xC1 };	// mov rcx, XYZ
		SafePushCode(rgcodeMovRcx, _countof(rgcodeMovRcx));

		SafePushCode(&clocalsNoArgs, sizeof(clocalsNoArgs));

		static const uint8_t rgcodeRepStos[] = { 0xF3, 0x48, 0xAB };	// rep stos qword ptr [rdx]
		SafePushCode(rgcodeRepStos, _countof(rgcodeRepStos));

		static const uint8_t rgcodeRestoreRdi[] = { 0x48, 0x89, 0xD7 };
		SafePushCode(rgcodeRestoreRdi);
	}
	_PushExpandStack();
	// push rdi
	SafePushCode(uint8_t(0x57));
}

void JitWriter::LogicOp(LogicOperation op)
{
	bool fSwapParams = false;
	switch (op)
	{
	case LogicOperation::ShiftLeft:
	case LogicOperation::ShiftRight:
	case LogicOperation::ShiftRightUnsigned:
	case LogicOperation::RotateLeft:
	case LogicOperation::RotateRight:
		fSwapParams = true;
	}
	_PopSecondParam(fSwapParams);
	const char *rgcode = nullptr;
	switch (op)
	{
	case LogicOperation::And:
		// and eax, ecx
		rgcode = "\x21\xC8";
		break;
	case LogicOperation::Or:
		// or eax, ecx
		rgcode = "\x09\xC8";
		break;
	case LogicOperation::Xor:
		// xor eax, ecx
		rgcode = "\x31\xC8";
		break;
	case LogicOperation::ShiftLeft:
		// shl eax, cl
		rgcode = "\xD3\xE0";
		break;
	case LogicOperation::ShiftRight:
		// sar eax, cl
		rgcode = "\xD3\xF8";
		break;
	case LogicOperation::ShiftRightUnsigned:
		// shr eax, cl
		rgcode = "\xD3\xE8";
		break;
	case LogicOperation::RotateLeft:
		// rol eax, cl
		rgcode = "\xD3\xC0";
		break;
	case LogicOperation::RotateRight:
		// ror eax, cl
		rgcode = "\xD3\xC8";
		break;

	default:
		Verify(false);
	}
	SafePushCode(rgcode, strlen(rgcode));
}

void JitWriter::LogicOp64(LogicOperation op)
{
	bool fSwapParams = false;
	switch (op)
	{
	case LogicOperation::ShiftLeft:
	case LogicOperation::ShiftRight:
	case LogicOperation::ShiftRightUnsigned:
	case LogicOperation::RotateLeft:
	case LogicOperation::RotateRight:
		fSwapParams = true;
	}
	_PopSecondParam(fSwapParams);
	const char *rgcode = nullptr;
	switch (op)
	{
	case LogicOperation::And:
		// and rax, rcx
		rgcode = "\x48\x21\xC8";
		break;
	case LogicOperation::Or:
		// or rax, rcx
		rgcode = "\x48\x09\xC8";
		break;
	case LogicOperation::Xor:
		// xor rax, rcx
		rgcode = "\x48\x31\xC8";
		break;
	case LogicOperation::ShiftLeft:
		// shl rax, cl
		rgcode = "\x48\xD3\xE0";
		break;
	case LogicOperation::ShiftRight:
		// sar rax, cl
		rgcode = "\x48\xD3\xF8";
		break;
	case LogicOperation::ShiftRightUnsigned:
		// shr rax, cl
		rgcode = "\x48\xD3\xE8";
		break;
	case LogicOperation::RotateLeft:
		rgcode = "\x48\xD3\xC0";
		break;
	case LogicOperation::RotateRight:
		rgcode = "\x48\xD3\xC8";
		break;
	default:
		Verify(false);
	}
	SafePushCode(rgcode, strlen(rgcode));
}

void JitWriter::Select()
{
	//	sub rdi, 16			; pop 2 vals from stack
	//	xor rcx, rcx		; zero our index
	//	test eax, eax		; test the conditional
	//	jnz FirstArg		; jump if index should be zero
	//	add rcx, 8			; else index should be 8 (bytes)
	// FirstArg:
	//	mov rax, [rdi+rcx]	; load the value
	static const uint8_t rgcode[] = { 0x48, 0x83, 0xEF, 0x10, 0x48, 0x31, 0xC9, 0x85, 0xC0, 0x75, 0x04, 0x48, 0x83, 0xC1, 0x08, 0x48, 0x8B, 0x04, 0x0F };
	SafePushCode(rgcode, _countof(rgcode));
}

void JitWriter::FnEpilogue(bool /*fRetVal*/)
{
#if 0	// now that we push rdi in the prologue we don't need this
	if (fRetVal)
	{
		// sub rdi, 8
		static const uint8_t rgcodeSub[] = { 0x48, 0x83, 0xEF, 0x08 };
		SafePushCode(rgcodeSub);
	}
#endif
	// ret
	static const uint8_t rgcode[] = { 0xC3 };
	SafePushCode(rgcode, _countof(rgcode));
}

void JitWriter::_SetDbgReg(uint32_t op)
{
	// mov r12, op
	static const uint8_t rgcode[] = { 0x49, 0xC7, 0xC4 };
	SafePushCode(rgcode, _countof(rgcode));
	SafePushCode(&op, sizeof(op));
}

void JitWriter::Mul32()
{
	// mul dword ptr [rdi-8]		; note: clobbers edx
	// sub rdi, 8
	static const uint8_t rgcode[] = { 0xF7, 0x67, 0xF8, 0x48, 0x83, 0xEF, 0x08 };
	SafePushCode(rgcode);
}


void JitWriter::Mul64()
{
	// mul qword ptr [rdi-8]		; note: clobbers rdx
	// sub rdi, 8
	static const uint8_t rgcode[] = { 0x48, 0xF7, 0x67, 0xF8, 0x48, 0x83, 0xEF, 0x08 };
	SafePushCode(rgcode);
}


void JitWriter::BranchTableParse(const uint8_t **ppoperand, size_t *pcbOperand, const std::vector<std::pair<value_type, void*>> &stackBlockTypeAddr, std::vector<std::vector<int32_t*>> &stackVecFixups, std::vector<std::vector<void**>> &stackVecFixupsAbsolute)
{
	uint32_t target_count = safe_read_buffer<varuint32>(ppoperand, pcbOperand);
	std::vector<uint32_t> vectargets;
	vectargets.reserve(target_count);
	for (uint32_t itarget = 0; itarget < target_count; ++itarget)
	{
		vectargets.push_back(safe_read_buffer<varuint32>(ppoperand, pcbOperand));
	}
	uint32_t default_target = safe_read_buffer<varuint32>(ppoperand, pcbOperand);

	auto &pairBlockDft = *(stackBlockTypeAddr.rbegin() + default_target);
	bool fRetVal = (pairBlockDft.first != value_type::empty_block);

	// Setup the parameters and call thr BranchTable helper
	//	rcx - table pointer
	//
	//	lea rcx, [rip+distance_to_branch_table]		{ 0x48, 0x8D, 0x0D, rel32 }
	//	jmp [m_pfnBranchTable]						{ 0xFF, 0x25, rel32 }
	int32_t branchtable_offset = 6;
	static const uint8_t rgcodeLea[] = { 0x48, 0x8D, 0x0D };
	SafePushCode(rgcodeLea);
	SafePushCode(branchtable_offset);
	
	static const uint8_t rgcodeJmp[] = { 0xFF, 0x25 };
	SafePushCode(rgcodeJmp);
	ptrdiff_t diffFn = reinterpret_cast<ptrdiff_t>(m_pfnBranchTable) - reinterpret_cast<ptrdiff_t>(m_pexecPlaneCur + 4);
	Verify(diffFn == static_cast<int32_t>(diffFn));
	SafePushCode(static_cast<int32_t>(diffFn));

	// Now write the branch table
	/*
	;	Table Format:
	;		dword count
	;		dword fReturnVal
	;		--default_target--
	;		qword distance (how many blocks we're jumping)
	;		qword target_addr
	;		--table_targets---
	;		.... * count
	*/
	// Header
	SafePushCode(target_count);
	SafePushCode(int32_t(fRetVal));
	// default target
	SafePushCode(uint64_t(default_target));
	SafePushCode(pairBlockDft.second);
	if (pairBlockDft.second == nullptr)
		(stackVecFixupsAbsolute.rbegin() + default_target)->push_back(reinterpret_cast<void**>(m_pexecPlaneCur) - 1);
	// table targets
	for (uint32_t itarget = 0; itarget < target_count; ++itarget)
	{
		uint64_t target = vectargets[itarget];
		SafePushCode(target);
		auto &pairBlock = *(stackBlockTypeAddr.rbegin() + target);
		SafePushCode(pairBlock.second);
		if (pairBlock.second == nullptr)
			(stackVecFixupsAbsolute.rbegin() + target)->push_back(reinterpret_cast<void**>(m_pexecPlaneCur) - 1);
	}
}

void JitWriter::FloatArithmetic(ArithmeticOperation op, bool fDouble)
{
	// sub rdi, 8
	static const uint8_t rgcodeSubRdi[] = { 0x48, 0x83, 0xEF, 0x08 };
	SafePushCode(rgcodeSubRdi);
	if (fDouble)
	{
		// movq xmm1, rax
		// movq xmm0, qword ptr [rdi]
		static const uint8_t rgcodeSetup[] = { 0x66, 0x48, 0x0F, 0x6E, 0xC8, 0xF3, 0x0F, 0x7E, 0x07 };
		SafePushCode(rgcodeSetup);
	}
	else
	{
		// movd xmm1, eax
		// movd xmm0, dword ptr [rdi]
		static const uint8_t rgcodeSetup[] = { 0x66, 0x0F, 0x6E, 0xC8, 0x66, 0x0F, 0x6E, 0x07 };
		SafePushCode(rgcodeSetup);
	}

	const char *szCode = nullptr;
	switch (op)
	{
	case ArithmeticOperation::Add:
		if (fDouble)
			szCode = "\xF2\x0F\x58\xC1";	// addsd xmm0, xmm1
		else
			szCode = "\xF3\x0F\x58\xC1";	// addss xmm0, xmm1
		break;
	case ArithmeticOperation::Sub:
		if (fDouble)
			szCode = "\xF2\x0F\x5C\xC1";	// subsd xmm0, xmm1
		else
			szCode = "\xF3\x0F\x5C\xC1";	// subss xmm0, xmm1
		break;
	case ArithmeticOperation::Multiply:
		if (fDouble)
			szCode = "\xF2\x0F\x59\xC1";	// mulsd xmm0, xmm1
		else
			szCode = "\xF3\x0F\x59\xC1";	// mulss xmm0, xmm1
		break;
	case ArithmeticOperation::Divide:
		if (fDouble)
			szCode = "\xF2\x0F\x5E\xC1";	// divsd xmm0, xmm1
		else
			szCode = "\xF3\x0F\x5E\xC1";	// divss xmm0, xmm1
		break;
	}
	Verify(szCode != nullptr);
	SafePushCode(szCode, strlen(szCode));

	if (fDouble)
	{
		// movq rax, xmm0
		static const uint8_t rgcode[] = { 0x66, 0x48, 0x0F, 0x7E, 0xC0 };
		SafePushCode(rgcode);
	}
	else
	{
		// movd eax, xmm0
		static const uint8_t rgcode[] = { 0x66, 0x0F, 0x7E, 0xC0 };
		SafePushCode(rgcode);
	}
}


void JitWriter::GetGlobal(uint32_t idx)
{
	auto &glbl = m_pctxt->m_vecglbls.at(idx);

	if (glbl.fMutable)
	{
		_PushExpandStack();
		const char *szCode = nullptr;
		switch (glbl.type)
		{
		case value_type::f32:
		case value_type::i32:
			// mov eax, [m_pGlobalsStart + idx*8] (rip relative)
			szCode = "\x8B\x05";
			break;

		case value_type::f64:
		case value_type::i64:
			// mov rax, [m_pGlobalsStart + idx*8] (rip relative)
			szCode = "\x48\x8B\x05";
			break;

		default:
			Verify(false);
		}
		SafePushCode(szCode, strlen(szCode));
		// now push the address offset
		int64_t offset64 = reinterpret_cast<uint8_t*>(m_pGlobalsStart + idx) - (m_pexecPlaneCur + 4);
		int32_t offset = static_cast<int32_t>(offset64);
		Verify(offset64 == offset);
		SafePushCode(offset);
	}
	else
	{
		switch (glbl.type)
		{
		case value_type::f32:
		case value_type::i32:
			PushC32((uint32_t)glbl.val);
			break;
		case value_type::f64:
		case value_type::i64:
			PushC64(glbl.val);
			break;
		default:
			Verify(false);
		}
	}
}

void JitWriter::SetGlobal(uint32_t idx)
{
	auto &glbl = m_pctxt->m_vecglbls.at(idx);

	Verify(glbl.fMutable);
	const char *szCode = nullptr;
	switch (glbl.type)
	{
	case value_type::f32:
	case value_type::i32:
		szCode = "\x89\x05";
		break;

	case value_type::f64:
	case value_type::i64:
		szCode = "\x48\x89\x05";
		break;

	default:
		Verify(false);
	}
	SafePushCode(szCode, strlen(szCode));
	int64_t offset64 = reinterpret_cast<uint8_t*>(m_pGlobalsStart + idx) - (m_pexecPlaneCur + 4);
	int32_t offset = static_cast<int32_t>(offset64);
	Verify(offset64 == offset);
	SafePushCode(offset);
	_PopContractStack();
}

void JitWriter::ExtendSigned32_64()
{
	// mov [rdi], eax
	// movsxd rax, dword ptr [rdi]
	static const uint8_t rgcode[] = { 0x89, 0x07, 0x48, 0x63, 0x07 };
	SafePushCode(rgcode);
}

void JitWriter::CountTrailingZeros(bool f64)
{
	if (f64)
	{
		// tzcnt rax, rax
		static const uint8_t rgcode[] = { 0xF3, 0x48, 0x0F, 0xBC, 0xC0 };
		SafePushCode(rgcode);
	}
	else
	{
		// tzcnt eax, eax
		static const uint8_t rgcode[] = { 0xF3, 0x0F, 0xBC, 0xC0 };
		SafePushCode(rgcode);
	}
}

void JitWriter::CountLeadingZeros(bool f64)
{
	if (f64)
	{
		// lzcnt rax, rax
		static const uint8_t rgcode[] = { 0xF3, 0x48, 0x0F, 0xBD, 0xC0 };
		SafePushCode(rgcode);
	}
	else
	{
		// lzcnt eax, eax
		static const uint8_t rgcode[] = { 0xF3, 0x0F, 0xBD, 0xC0 };
		SafePushCode(rgcode);
	}
}

void JitWriter::Div(bool fSigned, bool fModulo, bool f64)
{
	if (fSigned && fModulo)
	{
		// To satisify the wasm spec and prevent overflow exceptions convert the divisor to its absolute value
		//  do this before the xchg below so the divisor is already in eax

		// Fancy branchless abs(eax):
		//		cdq
		//		xor eax, edx
		//		sub eax, edx
		if (f64)
		{
			static const uint8_t rgcodeFancy[] = { 0x48, 0x99, 0x48, 0x31, 0xD0, 0x48, 0x29, 0xD0 };
			SafePushCode(rgcodeFancy);
		}
		else
		{
			static const uint8_t rgcodeFancy[] = { 0x99, 0x31, 0xD0, 0x29, 0xD0 };
			SafePushCode(rgcodeFancy);
		}
	}

	// sub rdi, 8 - pop off one of our operands.  Do this early so we don't have to offset our memory accesses below
	static const uint8_t rgcodeSubRdi[] = { 0x48, 0x83, 0xEF, 0x08 };
	SafePushCode(rgcodeSubRdi);

	if (f64)
	{
		//	xchg rax, qword ptr [rdi]		; operands in the wrong order
		static const uint8_t rgcodeXchg[] = { 0x48, 0x87, 0x07 };
		SafePushCode(rgcodeXchg);
	}
	else
	{
		//	xchg eax, dword ptr [rdi]		; operands in the wrong order
		static const uint8_t rgcodeXchg[] = { 0x87, 0x07 };
		SafePushCode(rgcodeXchg);
	}
	if (fSigned)
	{
		if (f64)
		{
			static const uint8_t rgcodeCqo[] = { 0x48, 0x99 };	// cqo to load rdx with the sign extension
			SafePushCode(rgcodeCqo);
		}
		else
		{
			SafePushCode(uint8_t(0x99));	// cdq to load edx with the sign extension
		}
	}
	else
	{
		// xor edx, edx { 0x31, 0xD2 }
		static const uint8_t rgcodeClearEdx[] = { 0x31, 0xD2 };
		SafePushCode(rgcodeClearEdx);
	}

	if (f64)
		SafePushCode(uint8_t(0x48));	// rex prefix to make the following div instruction a 64-bit op
	if (fSigned)
	{
		//  idiv dword ptr [rdi]
		static const uint8_t rgcodeDiv[] = { 0xF7, 0x3F };
		SafePushCode(rgcodeDiv);
	}
	else
	{
		// div dword ptr [rdi]
		static const uint8_t rgcodeDiv[] = { 0xF7, 0x37 };
		SafePushCode(rgcodeDiv);
	}

	if (fModulo)
	{
		if (f64)
			SafePushCode(uint8_t(0x48));	// REX prefix to convert to 64-bit registers below
		// take the remainder that's in edx
		// mov eax, edx { 0x89, 0xD0 }
		static const uint8_t rgcodeMovEaxEdx[] = { 0x89, 0xD0 };
		SafePushCode(rgcodeMovEaxEdx);
	}
}

void JitWriter::CompileFn(uint32_t ifn)
{
	size_t cfnImports = 0;
	Verify(ifn >= m_pctxt->m_vecimports.size(), "Attempt to compile an import");
	FunctionCodeEntry *pfnc = m_pctxt->m_vecfn_code[ifn - m_pctxt->m_vecimports.size()].get();
	const uint8_t *pop = pfnc->vecbytecode.data();
	size_t cb = pfnc->vecbytecode.size();
	std::vector<std::pair<value_type, void*>> stackBlockTypeAddr;
	std::vector<std::vector<int32_t*>> stackVecFixupsRelative;
	std::vector<std::vector<void**>> stackVecFixupsAbsolute;

	reinterpret_cast<void**>(m_pexecPlane)[ifn] = m_pexecPlaneCur;	// set our entry in the vector table

	size_t itype = m_pctxt->m_vecfn_entries[ifn];
	uint32_t cparams = m_pctxt->m_vecfn_types[itype]->cparams;
	uint32_t clocals = cparams;
	for (size_t ilocalInfo = 0; ilocalInfo < pfnc->clocalVars; ++ilocalInfo)
	{
		clocals += pfnc->rglocals[ilocalInfo].count;
	}

	std::vector<uint32_t> vecifnCompile;

	FnPrologue(clocals, cparams);

	const char *szFnName = nullptr;
	for (size_t iexport = 0; iexport < m_pctxt->m_vecexports.size(); ++iexport)
	{
		if (m_pctxt->m_vecexports[iexport].kind == external_kind::Function)
		{
			if (m_pctxt->m_vecexports[iexport].index == ifn)
			{
				szFnName = m_pctxt->m_vecexports[iexport].strName.c_str();
				break;
			}
		}
	}

#ifdef PRINT_DISASSEMBLY
	if (szFnName != nullptr)
		printf("Function %s (%d):\n", szFnName, ifn);
	else
		printf("Function %d:\n", ifn);
#endif

	stackBlockTypeAddr.push_back(std::make_pair(value_type::none, nullptr));	// nullptr means we need to fixup addrs
	stackVecFixupsRelative.push_back(std::vector<int32_t*>());
	stackVecFixupsAbsolute.push_back(std::vector<void**>());
	while (cb > 0)
	{
		cb--;	// count *pop
		++pop;
		_SetDbgReg(*(pop - 1));
#ifdef PRINT_DISASSEMBLY
		printf("%p (%X):\t", m_pexecPlaneCur, *(pop - 1));
		for (size_t itab = 0; itab < stackBlockTypeAddr.size(); ++itab)
			printf("\t");
#endif
		switch ((opcode)*(pop - 1))
		{
		case opcode::unreachable:
		{
			// ud2
#ifdef PRINT_DISASSEMBLY
			printf("unreachable\n");
#endif
			static const uint8_t rgcode[] = { 0x0F, 0x0B };
			SafePushCode(rgcode, _countof(rgcode));
			break;
		}

		case opcode::nop:
#ifdef PRINT_DISASSEMBLY
			printf("nop\n");
#endif
			break;

		case opcode::block:
		{
			value_type type = safe_read_buffer<value_type>(&pop, &cb);
#ifdef PRINT_DISASSEMBLY
			printf("block\n");
#endif
			stackBlockTypeAddr.push_back(std::make_pair(type, nullptr));	// nullptr means we need to fixup addrs
			stackVecFixupsRelative.push_back(std::vector<int32_t*>());
			stackVecFixupsAbsolute.push_back(std::vector<void**>());
			EnterBlock();
			break;
		}

		case opcode::loop:
		{
			value_type type = safe_read_buffer<value_type>(&pop, &cb);
#ifdef PRINT_DISASSEMBLY
			printf("loop\n");
#endif
			stackBlockTypeAddr.push_back(std::make_pair(type, m_pexecPlaneCur));
			stackVecFixupsRelative.push_back(std::vector<int32_t*>());
			stackVecFixupsAbsolute.push_back(std::vector<void**>());
			EnterBlock();
			break;
		}

		case opcode::IF:
		{
			value_type type = safe_read_buffer<value_type>(&pop, &cb);
#ifdef PRINT_DISASSEMBLY
			printf("if\n");
#endif
			stackBlockTypeAddr.push_back(std::make_pair(type, nullptr));	// nullptr means we need to fixup addrs
			stackVecFixupsRelative.push_back(std::vector<int32_t*>());
			stackVecFixupsAbsolute.push_back(std::vector<void**>());
			int32_t *pifFix = EnterIF();
			(stackVecFixupsRelative.rbegin())->push_back(pifFix);
			break;
		}

		case opcode::ELSE:
		{
#ifdef PRINT_DISASSEMBLY
			printf("ELSE\n");
#endif
			Verify(stackVecFixupsRelative.back().size() > 0);
			LeaveBlock(true);
			int32_t *prel32End = Jump(nullptr);	// if we got here its from the IF block above so jump to the end
			(stackVecFixupsRelative.rbegin())->push_back(prel32End);
			// Fixup the else pointer to go here (only the first, all others still branch to the end)
			int32_t *poffsetFix = stackVecFixupsRelative.back().front();
			stackVecFixupsRelative.back().erase(stackVecFixupsRelative.back().begin());	// remove it
			*poffsetFix = numeric_cast<int32_t>(m_pexecPlaneCur - (reinterpret_cast<uint8_t*>(poffsetFix) + sizeof(*poffsetFix)));
			_PushExpandStack();
			// push rdi
			static const uint8_t rgcode[] = { 0x57 };
			SafePushCode(rgcode);
			break;
		}

		case opcode::br:
		{
			uint32_t depth = safe_read_buffer<varuint32>(&pop, &cb);
#ifdef PRINT_DISASSEMBLY
			printf("br %u\n", depth);
#endif
			Verify(depth < stackBlockTypeAddr.size());
			auto &pairBlock = *(stackBlockTypeAddr.rbegin() + depth);

			// leave intermediate blocks (lie that we have a return so we don't do useless stack operations)
			for (uint32_t idepth = 0; idepth < depth; ++idepth)
			{
				LeaveBlock(true);
			}
			LeaveBlock(pairBlock.first != value_type::empty_block);

			int32_t *pdeltaFix = Jump(pairBlock.second);
			if (pairBlock.second == nullptr)
			{
				(stackVecFixupsRelative.rbegin() + depth)->push_back(pdeltaFix);
			}
			break;
		}
		case opcode::br_if:
		{
			uint32_t depth = safe_read_buffer<varuint32>(&pop, &cb);
#ifdef PRINT_DISASSEMBLY
			printf("br_if %u\n", depth);
#endif
			Verify(depth < stackBlockTypeAddr.size());
			auto &pairBlock = *(stackBlockTypeAddr.rbegin() + depth);

			int32_t *pdeltaNoJmp = JumpNIf(nullptr);	// skip everything if we won't jump
			// leave intermediate blocks (lie that we have a return so we don't do useless stack operations)
			for (uint32_t idepth = 0; idepth < depth; ++idepth)
			{
				LeaveBlock(true);
			}
			LeaveBlock(pairBlock.first != value_type::empty_block);

			int32_t *pdeltaFix = Jump(pairBlock.second);
			if (pairBlock.second == nullptr)
			{
				(stackVecFixupsRelative.rbegin() + depth)->push_back(pdeltaFix);
			}
			*pdeltaNoJmp = numeric_cast<int32_t>(m_pexecPlaneCur - (reinterpret_cast<uint8_t*>(pdeltaNoJmp) + sizeof(*pdeltaNoJmp)));
			break;
		}
		case opcode::br_table:
		{
#ifdef PRINT_DISASSEMBLY
			printf("br_table\n");
#endif
			BranchTableParse(&pop, &cb, stackBlockTypeAddr, stackVecFixupsRelative, stackVecFixupsAbsolute);
			break;
		}

		case opcode::ret:
		{
#ifdef PRINT_DISASSEMBLY
			printf("return\n");
#endif
			// add rsp, (cblock * 8)
			static const uint8_t rgcode[] = { 0x48, 0x81, 0xC4 };
			int32_t cbSub = numeric_cast<int32_t>(stackVecFixupsRelative.size() * 8);
			SafePushCode(rgcode);
			SafePushCode(cbSub);
			FnEpilogue(m_pctxt->m_vecfn_types[itype]->fHasReturnValue);
			break;
		}

		case opcode::call:
		{
			uint32_t idx = safe_read_buffer<varuint32>(&pop, &cb);
#ifdef PRINT_DISASSEMBLY
			printf("call %d\n", idx);
#endif
			Verify(idx < m_cfn);
			vecifnCompile.push_back(idx);
			auto ptype = m_pctxt->m_vecfn_types.at(m_pctxt->m_vecfn_entries.at(idx)).get();
			CallIfn(idx, clocals, ptype->cparams, ptype->fHasReturnValue, false /*fIndirect*/);
			break;
		}
		case opcode::call_indirect:
		{
#ifdef PRINT_DISASSEMBLY
			printf("call_indirect\n");
#endif
			uint32_t idx = safe_read_buffer<varuint32>(&pop, &cb);
			safe_read_buffer<char>(&pop, &cb);	// reserved
			auto ptype = m_pctxt->m_vecfn_types.at(idx).get();
			CallIfn(m_pctxt->ITypeCanonicalFromIType(idx), clocals, ptype->cparams, ptype->fHasReturnValue, true /*fIndirect*/);
			break;
		}

		case opcode::drop:
		{
#ifdef PRINT_DISASSEMBLY
			printf("drop\n");
#endif
			_PopContractStack();
			break;
		}
		case opcode::select:
		{
#ifdef PRINT_DISASSEMBLY
			printf("select\n");
#endif
			Select();
			break;
		}

		case opcode::i32_const:
		{
			uint32_t val = safe_read_buffer<varint32>(&pop, &cb);
#ifdef PRINT_DISASSEMBLY
			printf("i32.const %d\n", val);
#endif
			PushC32(val);
			break;
		}
		case opcode::i64_const:
		{
			uint64_t val = safe_read_buffer<varint64>(&pop, &cb);
#ifdef PRINT_DISASSEMBLY
			printf("i64.const %llu\n", val);
#endif
			PushC64(val);
			break;
		}
		case opcode::f32_const:
		{
			float val = safe_read_buffer<float>(&pop, &cb);
#ifdef PRINT_DISASSEMBLY
			printf("f32.const %f\n", (double)val);
#endif
			PushF32(val);
			break;
		}
		case opcode::f64_const:
		{
			double val = safe_read_buffer<double>(&pop, &cb);
#ifdef PRINT_DISASSEMBLY
			printf("f64.const %f\n", val);
#endif
			PushF64(val);
			break;
		}
		case opcode::i32_eqz:
#ifdef PRINT_DISASSEMBLY
			printf("i32.eqz\n");
#endif
			Eqz32();
			break;
		case opcode::i32_eq:
#ifdef PRINT_DISASSEMBLY
			printf("i32.eq\n");
#endif
			Compare(CompareType::Equal, false /*fSigned*/, false /*f64*/);	// Note: signedness is irrelevant
			break;
		case opcode::i32_ne:
#ifdef PRINT_DISASSEMBLY
			printf("i32.ne\n");
#endif
			Compare(CompareType::NotEqual, false /*fSigned*/, false /*f64*/); // Note: signedness is irrelevant
			break;
		case opcode::i32_lt_s:
#ifdef PRINT_DISASSEMBLY
			printf("i32.lt_s\n");
#endif
			Compare(CompareType::LessThan, true /*fSigned*/, false /*f64*/);
			break;
		case opcode::i32_lt_u:
#ifdef PRINT_DISASSEMBLY
			printf("i32.lt_u\n");
#endif
			Compare(CompareType::LessThan, false /*fSigned*/, false /*f64*/);
			break;
		case opcode::i32_gt_s:
#ifdef PRINT_DISASSEMBLY
			printf("i32.gt_s\n");
#endif
			Compare(CompareType::GreaterThan, true /*fSigned*/, false /*f64*/);
			break;
		case opcode::i32_gt_u:
#ifdef PRINT_DISASSEMBLY
			printf("i32.gt_u\n");
#endif
			Compare(CompareType::GreaterThan, false /*fSigned*/, false /*f64*/);
			break;
		case opcode::i32_le_s:
#ifdef PRINT_DISASSEMBLY
			printf("i32_le_s\n");
#endif
			Compare(CompareType::LessThanEqual, true /*fSigned*/, false /*f64*/);
			break;
		case opcode::i32_le_u:
#ifdef PRINT_DISASSEMBLY
			printf("i32_le_u\n");
#endif
			Compare(CompareType::LessThanEqual, false /*fSigned*/, false /*f64*/);
			break;
		case opcode::i32_ge_s:
#ifdef PRINT_DISASSEMBLY
			printf("i32_ge_s\n");
#endif
			Compare(CompareType::GreaterThanEqual, true /*fSigned*/, false /*f64*/);
			break;
		case opcode::i32_ge_u:
#ifdef PRINT_DISASSEMBLY
			printf("i32_ge_u\n");
#endif
			Compare(CompareType::GreaterThanEqual, false /*fSigned*/, false /*f64*/);
			break;

		case opcode::i64_eqz:
#ifdef PRINT_DISASSEMBLY
			printf("i64.eqz\n");
#endif
			Eqz64();
			break;
		case opcode::i64_eq:
#ifdef PRINT_DISASSEMBLY
			printf("i64.eq\n");
#endif
			Compare(CompareType::Equal, false /*fSigned*/, true /*f64*/);
			break;
		case opcode::i64_ne:
#ifdef PRINT_DISASSEMBLY
			printf("i64.ne\n");
#endif
			Compare(CompareType::NotEqual, false /*fSigned*/, true /*f64*/);
			break;
		case opcode::i64_lt_s:
#ifdef PRINT_DISASSEMBLY
			printf("i64.lt_s\n");
#endif
			Compare(CompareType::LessThan, true /*fSigned*/, true /*f64*/);
			break;
		case opcode::i64_lt_u:
#ifdef PRINT_DISASSEMBLY
			printf("i64.lt_u\n");
#endif
			Compare(CompareType::LessThan, false /*fSigned*/, true /*f64*/);
			break;
		case opcode::i64_gt_s:
#ifdef PRINT_DISASSEMBLY
			printf("i64.gt_s\n");
#endif
			Compare(CompareType::GreaterThan, true /*fSigned*/, true /*f64*/);
			break;
		case opcode::i64_gt_u:
#ifdef PRINT_DISASSEMBLY
			printf("i64.gt_u\n");
#endif
			Compare(CompareType::GreaterThan, false /*fSigned*/, true /*f64*/);
			break;
		case opcode::i64_le_s:
#ifdef PRINT_DISASSEMBLY
			printf("i64.le_s\n");
#endif
			Compare(CompareType::LessThanEqual, true /*fSigned*/, true /*f64*/);
			break;
		case opcode::i64_le_u:
#ifdef PRINT_DISASSEMBLY
			printf("i64.le_u\n");
#endif
			Compare(CompareType::LessThanEqual, false /*fSigned*/, true /*f64*/);
			break;
		case opcode::i64_ge_s:
#ifdef PRINT_DISASSEMBLY
			printf("i64.ge_s\n");
#endif
			Compare(CompareType::GreaterThanEqual, true /*fSigned*/, true /*f64*/);
			break;
		case opcode::i64_ge_u:
#ifdef PRINT_DISASSEMBLY
			printf("i64.ge_u\n");
#endif
			Compare(CompareType::GreaterThanEqual, false /*fSigned*/, true /*f64*/);
			break;

		case opcode::f32_eq:
#ifdef PRINT_DISASSEMBLY
			printf("f32.eq\n");
#endif
			FloatCompare(CompareType::Equal);
			break;
		case opcode::f32_ne:
#ifdef PRINT_DISASSEMBLY
			printf("f32.ne\n");
#endif
			FloatCompare(CompareType::NotEqual);
			break;
		case opcode::f32_lt:
#ifdef PRINT_DISASSEMBLY
			printf("f32.lt\n");
#endif
			FloatCompare(CompareType::LessThan);
			break;
		case opcode::f32_gt:
#ifdef PRINT_DISASSEMBLY
			printf("f32.gt\n");
#endif
			FloatCompare(CompareType::GreaterThan);
			break;
		case opcode::f32_le:
#ifdef PRINT_DISASSEMBLY
			printf("f32.le\n");
#endif
			FloatCompare(CompareType::LessThanEqual);
			break;
		case opcode::f32_ge:
#ifdef PRINT_DISASSEMBLY
			printf("f32.ge\n");
#endif
			FloatCompare(CompareType::GreaterThanEqual);
			break;

		case opcode::f64_eq:
#ifdef PRINT_DISASSEMBLY
			printf("f64.eq\n");
#endif
			DoubleCompare(CompareType::Equal);
			break;
		case opcode::f64_ne:
#ifdef PRINT_DISASSEMBLY
			printf("f64.ne\n");
#endif
			DoubleCompare(CompareType::NotEqual);
			break;
		case opcode::f64_lt:
#ifdef PRINT_DISASSEMBLY
			printf("f64.lt\n");
#endif
			DoubleCompare(CompareType::LessThan);
		case opcode::f64_gt:
#ifdef PRINT_DISASSEMBLY
			printf("f64.gt\n");
#endif
			DoubleCompare(CompareType::GreaterThan);
			break;
		case opcode::f64_le:
#ifdef PRINT_DISASSEMBLY
			printf("f64.le\n");
#endif
			DoubleCompare(CompareType::LessThanEqual);
			break;
		case opcode::f64_ge:
#ifdef PRINT_DISASSEMBLY
			printf("f64.ge\n");
#endif
			DoubleCompare(CompareType::GreaterThanEqual);
			break;

		case opcode::f32_load:
		case opcode::i64_load32_u:
		case opcode::i32_load:
		{
			uint32_t align = safe_read_buffer<varuint32>(&pop, &cb);	// NYI alignment
			uint32_t offset = safe_read_buffer<varuint32>(&pop, &cb);
#ifdef PRINT_DISASSEMBLY
			printf("i32.load $%X\n", offset);
#endif
			LoadMem(offset, false, 4, false);
			break;
		}

		case opcode::f64_load:
		{
			uint32_t align = safe_read_buffer<varuint32>(&pop, &cb);	// NYI alignment
			uint32_t offset = safe_read_buffer<varuint32>(&pop, &cb);
#ifdef PRINT_DISASSEMBLY
			printf("f64.load $%X\n", offset);
#endif
			LoadMem(offset, true, 8, false);
			break;
		}

		case opcode::i64_load8_u:
		case opcode::i32_load8_u:
		{
			uint32_t align = safe_read_buffer<varuint32>(&pop, &cb);	// NYI alignment
			uint32_t offset = safe_read_buffer<varuint32>(&pop, &cb);
#ifdef PRINT_DISASSEMBLY
			printf("i32_load8_u $%X\n", offset);
#endif
			LoadMem(offset, false, 1, false);
			break;
		}
		case opcode::i32_load8_s:
		{
			uint32_t align = safe_read_buffer<varuint32>(&pop, &cb);	// NYI alignment
			uint32_t offset = safe_read_buffer<varuint32>(&pop, &cb);
#ifdef PRINT_DISASSEMBLY
			printf("i32_load8_s $%X\n", offset);
#endif
			LoadMem(offset, false, 1, true);
			break;
		}
		case opcode::i32_load16_s:
		{
			uint32_t align = safe_read_buffer<varuint32>(&pop, &cb);	// NYI alignment
			uint32_t offset = safe_read_buffer<varuint32>(&pop, &cb);
#ifdef PRINT_DISASSEMBLY
			printf("i32_load16_s $%X\n", offset);
#endif
			LoadMem(offset, false, 2, true);
			break;
		}

		case opcode::i64_load:
		{
			uint32_t align = safe_read_buffer<varuint32>(&pop, &cb);	// NYI alignment
			uint32_t offset = safe_read_buffer<varuint32>(&pop, &cb);
#ifdef PRINT_DISASSEMBLY
			printf("i64.load $%X\n", offset);
#endif
			LoadMem(offset, true, 8, false);
			break;
		}

		case opcode::i64_load8_s:
		{
			uint32_t align = safe_read_buffer<varuint32>(&pop, &cb);	// NYI alignment
			uint32_t offset = safe_read_buffer<varuint32>(&pop, &cb);
#ifdef PRINT_DISASSEMBLY
			printf("i64.load8_s $%X\n", offset);
#endif
			LoadMem(offset, true, 1, true);
			break;
		}

		case opcode::i32_load16_u:
		case opcode::i64_load16_u:
		{
			uint32_t align = safe_read_buffer<varuint32>(&pop, &cb);	// NYI alignment
			uint32_t offset = safe_read_buffer<varuint32>(&pop, &cb);
#ifdef PRINT_DISASSEMBLY
			printf("i64/32.load16_u $%X\n", offset);
#endif
			LoadMem(offset, false, 2, false);
			break;
		}

		case opcode::i64_load16_s:
		{
			uint32_t align = safe_read_buffer<varuint32>(&pop, &cb);	// NYI alignment
			uint32_t offset = safe_read_buffer<varuint32>(&pop, &cb);
#ifdef PRINT_DISASSEMBLY
			printf("i64.load16_s $%X\n", offset);
#endif
			LoadMem(offset, true, 2, true);
			break;
		}

		case opcode::i64_load32_s:
		{
			uint32_t align = safe_read_buffer<varuint32>(&pop, &cb);	// NYI alignment
			uint32_t offset = safe_read_buffer<varuint32>(&pop, &cb);
#ifdef PRINT_DISASSEMBLY
			printf("i64_load32_s $%X\n", offset);
#endif
			LoadMem(offset, true, 4, true);
			break;
		}

		case opcode::get_local:
		{
			uint32_t idx = safe_read_buffer<varuint32>(&pop, &cb);
#ifdef PRINT_DISASSEMBLY
			printf("get_local $%X\n", idx);
#endif
			Verify(idx < clocals);
			GetLocal(idx);
			break;
		}
		case opcode::set_local:
		{
			uint32_t idx = safe_read_buffer<varuint32>(&pop, &cb);
#ifdef PRINT_DISASSEMBLY
			printf("set_local $%X\n", idx);
#endif
			Verify(idx < clocals);
			SetLocal(idx, true /*fPop*/);
			break;
		}
		case opcode::tee_local:
		{
			uint32_t idx = safe_read_buffer<varuint32>(&pop, &cb);
#ifdef PRINT_DISASSEMBLY
			printf("tee_local $%X\n", idx);
#endif
			Verify(idx < clocals);
			SetLocal(idx, false /*fPop*/);
			break;
		}
		case opcode::get_global:
		{
			uint32_t iglbl = safe_read_buffer<varuint32>(&pop, &cb);
#ifdef PRINT_DISASSEMBLY
			printf("get_global $%X\n", iglbl);
#endif
			GetGlobal(iglbl);
			break;
		}
		case opcode::set_global:
		{
			uint32_t iglbl = safe_read_buffer<varuint32>(&pop, &cb);
#ifdef PRINT_DISASSEMBLY
			printf("set_global $%X\n", iglbl);
#endif
			SetGlobal(iglbl);
			break;
		}

		case opcode::i64_store32:
		case opcode::i32_store:
		case opcode::f32_store:
		{
			uint32_t align = safe_read_buffer<varuint32>(&pop, &cb);	// NYI alignment
			uint32_t offset = safe_read_buffer<varuint32>(&pop, &cb);
#ifdef PRINT_DISASSEMBLY
			printf("i32.store $%X\n", offset);
#endif
			StoreMem(offset, 4);
			break;
		}

		case opcode::f64_store:
		case opcode::i64_store:
		{
			uint32_t align = safe_read_buffer<varuint32>(&pop, &cb);	// NYI alignment
			uint32_t offset = safe_read_buffer<varuint32>(&pop, &cb);
#ifdef PRINT_DISASSEMBLY
			printf("i64.store $%X\n", offset);
#endif
			StoreMem(offset, 8);
			break;
		}

		case opcode::i64_store16:
		case opcode::i32_store16:
		{
			uint32_t align = safe_read_buffer<varuint32>(&pop, &cb);	// NYI alignment
			uint32_t offset = safe_read_buffer<varuint32>(&pop, &cb);
#ifdef PRINT_DISASSEMBLY
			printf("i32.store16 $%X\n", offset);
#endif
			StoreMem(offset, 2);
			break;
		}

		case opcode::i64_store8:
		case opcode::i32_store8:
		{
			uint32_t align = safe_read_buffer<varuint32>(&pop, &cb);	// NYI alignment
			uint32_t offset = safe_read_buffer<varuint32>(&pop, &cb);
#ifdef PRINT_DISASSEMBLY
			printf("i32.store8 $%X\n", offset);
#endif
			StoreMem(offset, 1);
			break;
		}

		case opcode::i32_clz:
#ifdef PRINT_DISASSEMBLY
			printf("i32.clz");
#endif
			CountLeadingZeros(false /*f64*/);
			break;
		case opcode::i32_ctz:
#ifdef PRINT_DISASSEMBLY
			printf("i32.ctz\n");
#endif
			CountTrailingZeros(false /*f64*/);
			break;
		case opcode::i32_popcnt:
#ifdef PRINT_DISASSEMBLY
			printf("i32.popcnt\n");
#endif
			Popcnt32();
			break;
		case opcode::i32_add:
#ifdef PRINT_DISASSEMBLY
			printf("i32.add\n");
#endif
			Add32();
			break;
		case opcode::i32_sub:
#ifdef PRINT_DISASSEMBLY
			printf("i32.sub\n");
#endif
			Sub32();
			break;
		case opcode::i32_mul:
#ifdef PRINT_DISASSEMBLY
			printf("i32.mul\n");
#endif
			Mul32();
			break;
		case opcode::i32_div_s:
#ifdef PRINT_DISASSEMBLY
			printf("i32.div_s\n");
#endif
			Div(true /*fSigned*/, false /*fModulo*/, false /*f64*/);
			break;
		case opcode::i32_div_u:
#ifdef PRINT_DISASSEMBLY
			printf("i32.div_u\n");
#endif
			Div(false /*fSigned*/, false /*fModulo*/, false /*f64*/);
			break;
		case opcode::i32_rem_s:
#ifdef PRINT_DISASSEMBLY
			printf("i32.rem_s\n");
#endif
			Div(true /*fSigned*/, true /*fModulo*/, false /*f64*/);
			break;
		case opcode::i32_rem_u:
#ifdef PRINT_DISASSEMBLY
			printf("i32.rem_u\n");
#endif
			Div(false /*fSigned*/, true /*fModulo*/, false /*f64*/);
			break;
		case opcode::i32_and:
#ifdef PRINT_DISASSEMBLY
			printf("i32.and\n");
#endif
			LogicOp(LogicOperation::And);
			break;
		case opcode::i32_or:
#ifdef PRINT_DISASSEMBLY
			printf("i32.or\n");
#endif
			LogicOp(LogicOperation::Or);
			break;
		case opcode::i32_xor:
#ifdef PRINT_DISASSEMBLY
			printf("i32.xor\n");
#endif
			LogicOp(LogicOperation::Xor);
			break;
		case opcode::i32_shl:
#ifdef PRINT_DISASSEMBLY
			printf("i32.shl\n");
#endif
			LogicOp(LogicOperation::ShiftLeft);
			break;
		case opcode::i32_shr_s:
#ifdef PRINT_DISASSEMBLY
			printf("i32.shr_s\n");
#endif
			LogicOp(LogicOperation::ShiftRight);
			break;
		case opcode::i32_shr_u:
#ifdef PRINT_DISASSEMBLY
			printf("i32.shr_u\n");
#endif
			LogicOp(LogicOperation::ShiftRightUnsigned);
			break;
		case opcode::i32_rotl:
#ifdef PRINT_DISASSEMBLY
			printf("i32.rotl\n");
#endif
			LogicOp(LogicOperation::RotateLeft);
			break;
		case opcode::i32_rotr:
#ifdef PRINT_DISASSEMBLY
			printf("i32.rotr\n");
#endif
			LogicOp(LogicOperation::RotateRight);
			break;

		case opcode::i64_clz:
#ifdef PRINT_DISASSEMBLY
			printf("i64.clz\n");
#endif
			CountLeadingZeros(true /*f64*/);
			break;
		case opcode::i64_ctz:
#ifdef PRINT_DISASSEMBLY
			printf("i64.ctz\n");
#endif
			CountTrailingZeros(true /*f64*/);
			break;
		case opcode::i64_popcnt:
#ifdef PRINT_DISASSEMBLY
			printf("i64.popcnt\n");
#endif
			Popcnt64();
			break;
		case opcode::i64_add:
#ifdef PRINT_DISASSEMBLY
			printf("i64.add\n");
#endif
			Add64();
			break;
		case opcode::i64_sub:
#ifdef PRINT_DISASSEMBLY
			printf("i64.sub\n");
#endif
			Sub64();
			break;
		case opcode::i64_mul:
#ifdef PRINT_DISASSEMBLY
			printf("i64.mul\n");
#endif
			Mul64();
			break;
		case opcode::i64_div_s:
#ifdef PRINT_DISASSEMBLY
			printf("i64.div_s\n");
#endif
			Div(true /*fSigned*/, false /*fModulo*/, true /*f64*/);
			break;
		case opcode::i64_div_u:
#ifdef PRINT_DISASSEMBLY
			printf("i64.div_u\n");
#endif
			Div(false /*fSigned*/, false /*fModulo*/, true /*f64*/);
			break;
		case opcode::i64_rem_s:
#ifdef PRINT_DISASSEMBLY
			printf("i64.rem_s\n");
#endif
			Div(true /*fSigned*/, true /*fModulo*/, true /*f64*/);
			break;
		case opcode::i64_rem_u:
#ifdef PRINT_DISASSEMBLY
			printf("i64.rem_u\n");
#endif
			Div(false /*fSigned*/, true /*fModulo*/, true /*f64*/);
			break;
		case opcode::i64_and:
#ifdef PRINT_DISASSEMBLY
			printf("i64.and\n");
#endif
			LogicOp64(LogicOperation::And);
			break;
		case opcode::i64_or:
#ifdef PRINT_DISASSEMBLY
			printf("i64.or\n");
#endif
			LogicOp64(LogicOperation::Or);
			break;
		case opcode::i64_xor:
#ifdef PRINT_DISASSEMBLY
			printf("ix64.xor\n");
#endif
			LogicOp64(LogicOperation::Xor);
			break;
		case opcode::i64_shl:
#ifdef PRINT_DISASSEMBLY
			printf("i64.shl\n");
#endif
			LogicOp64(LogicOperation::ShiftLeft);
			break;
		case opcode::i64_shr_s:
#ifdef PRINT_DISASSEMBLY
			printf("i64.shr_s\n");
#endif
			LogicOp64(LogicOperation::ShiftRight);
			break;
		case opcode::i64_shr_u:
#ifdef PRINT_DISASSEMBLY
			printf("i64.shr_u\n");
#endif
			LogicOp64(LogicOperation::ShiftRightUnsigned);
			break;
		case opcode::i64_rotl:
#ifdef PRINT_DISASSEMBLY
			printf("i64.rotl\n");
#endif
			LogicOp64(LogicOperation::RotateLeft);
			break;
		case opcode::i64_rotr:
#ifdef PRINT_DISASSEMBLY
			printf("i64.rotr\n");
#endif
			LogicOp64(LogicOperation::RotateRight);
			break;

		case opcode::f32_neg:
#ifdef PRINT_DISASSEMBLY
			printf("f32.neg\n");
#endif
			FloatNeg(false /*fDouble*/);
			break;
		case opcode::f32_add:
#ifdef PRINT_DISASSEMBLY
			printf("f32.add\n");
#endif
			FloatArithmetic(ArithmeticOperation::Add, false /*fDouble*/);
			break;
		case opcode::f32_sub:
#ifdef PRINT_DISASSEMBLY
			printf("f32.sub\n");
#endif
			FloatArithmetic(ArithmeticOperation::Sub, false /*fDouble*/);
			break;
		case opcode::f32_mul:
#ifdef PRINT_DISASSEMBLY
			printf("f32.mul\n");
#endif
			FloatArithmetic(ArithmeticOperation::Multiply, false /*fDouble*/);
			break;
		case opcode::f32_div:
#ifdef PRINT_DISASSEMBLY
			printf("f32.div\n");
#endif
			FloatArithmetic(ArithmeticOperation::Divide, false /*fDouble*/);
			break;
		case opcode::f32_copysign:
			Ud2();	// doesn't work
#ifdef PRINT_DISASSEMBLY
			printf("f32.copysign\n");
#endif
			// and eax, 8000'0000h
			// xor [rdi - 8], eax
			SafePushCode("\x25\x00\x00\x00\x80\x31\x47\xF8", 8);
			_PopContractStack();
			break;

		case opcode::f64_neg:
#ifdef PRINT_DISASSEMBLY
			printf("f64.neg\n");
#endif
			FloatNeg(true /*fDouble*/);
			break;
		case opcode::f64_add:
#ifdef PRINT_DISASSEMBLY
			printf("f64.add\n");
#endif
			FloatArithmetic(ArithmeticOperation::Add, true /*fDouble*/);
			break;
		case opcode::f64_sub:
#ifdef PRINT_DISASSEMBLY
			printf("f64.sub\n");
#endif
			FloatArithmetic(ArithmeticOperation::Sub, true /*fDouble*/);
			break;
		case opcode::f64_mul:
#ifdef PRINT_DISASSEMBLY
			printf("f64.mul\n");
#endif
			FloatArithmetic(ArithmeticOperation::Multiply, true /*fDouble*/);
			break;
		case opcode::f64_div:
#ifdef PRINT_DISASSEMBLY
			printf("f64.div\n");
#endif
			FloatArithmetic(ArithmeticOperation::Divide, true /*fDouble*/);
			break;
		case opcode::f64_copysign:
#ifdef PRINT_DISASSEMBLY
			printf("f64.copysign\n");
#endif
			Ud2();	// doesn't work
			// mov rcx, 0x8000000000000000
			// and rax, rcx
			// xor[rdi - 8], rax
			SafePushCode("\x48\xB9\x00\x00\x00\x00\x00\x00\x00\x80\x48\x21\xC8\x48\x31\x47\xF8", 17);
			_PopContractStack();
			break;

		case opcode::i32_wrap_i64:
		{
			const uint8_t rgcode[] = { 0x89, 0xC0 };	// mov eax, eax	; truncates upper 64-bits
			SafePushCode(rgcode);
			break;
		}
		case opcode::i32_trunc_s_f32:
#ifdef PRINT_DISASSEMBLY
			printf("i32.trunc_s/f32\n");
#endif
			Convert(value_type::i32, value_type::f32, true /*fSigned*/);
			break;
		case opcode::i32_trunc_u_f32:
#ifdef PRINT_DISASSEMBLY
			printf("i32.trunc_u/f32\n");
#endif
			Convert(value_type::i32, value_type::f32, false /*fSigned*/);
			break;
		case opcode::i32_trunc_s_f64:
#ifdef PRINT_DISASSEMBLY
			printf("i32.trunc_s/f64\n");
#endif
			Convert(value_type::i32, value_type::f64, true /*fSigned*/);
			break;
		case opcode::i32_trunc_u_f64:
#ifdef PRINT_DISASSEMBLY
			printf("i32.trunc_u/f64\n");
#endif
			Convert(value_type::i32, value_type::f64, false /*fSigned*/);
			break;
		case opcode::i64_trunc_s_f32:
#ifdef PRINT_DISASSEMBLY
			printf("i64.trunc_s/f32\n");
#endif
			Convert(value_type::i64, value_type::f32, true /*fSigned*/);
			break;
		case opcode::i64_trunc_u_f32:
#ifdef PRINT_DISASSEMBLY
			printf("i64.trunc_u/f32");
#endif
			Convert(value_type::i64, value_type::f32, false /*fSigned*/);
			break;
		case opcode::i64_trunc_s_f64:
#ifdef PRINT_DISASSEMBLY
			printf("i64.trunc_s/f64\n");
#endif
			Convert(value_type::i64, value_type::f64, true /*fSigned*/);
			break;
		case opcode::i64_trunc_u_f64:
#ifdef PRINT_DISASSEMBLY
			printf("i64.trunc_u/f64\n");
#endif
			Convert(value_type::i64, value_type::f64, false /*fSigned*/);
			break;
		case opcode::i64_extend_s_i32:
#ifdef PRINT_DISASSEMBLY
			printf("i64.extend_s_i32\n");
#endif
			ExtendSigned32_64();
			break;
		case opcode::i64_extend_u_i32:
#ifdef PRINT_DISASSEMBLY
			printf("i64.extend_u/i32\n");
#endif
			break;	// NOP because the upper register should be zero regardless
		case opcode::f32_convert_s_i32:
#ifdef PRINT_DISASSEMBLY
			printf("f32.convert_s/i32\n");
#endif
			Convert(value_type::f32, value_type::i32, true /*fSigned*/);
			break;
		case opcode::f32_convert_u_i32:
#ifdef PRINT_DISASSEMBLY
			printf("f32.convert_u/i32\n");
#endif
			Convert(value_type::f32, value_type::i32, false /*fSigned*/);
			break;
		case opcode::f32_convert_s_i64:
#ifdef PRINT_DISASSEMBLY
			printf("f32.convert_s/i64\n");
#endif
			Convert(value_type::f32, value_type::i64, true /*fSigned*/);
			break;
		case opcode::f32_convert_u_i64:
#ifdef PRINT_DISASSEMBLY
			printf("f32.convert_u/i64\n");
#endif
			Convert(value_type::f32, value_type::i64, false /*fSigned*/);
			break;
		case opcode::f64_convert_s_i32:
#ifdef PRINT_DISASSEMBLY
			printf("f64.convert_s/i32\n");
#endif
			Convert(value_type::f64, value_type::i32, true /*fSigned*/);
			break;
		case opcode::f64_convert_u_i32:
#ifdef PRINT_DISASSEMBLY
			printf("f64.convert_u/i32\n");
#endif
			Convert(value_type::f64, value_type::i32, false /*fSigned*/);
			break;
		case opcode::f64_convert_s_i64:
#ifdef PRINT_DISASSEMBLY
			printf("f64.convert_s/i64\n");
#endif
			Convert(value_type::f64, value_type::i64, true /*fSigned*/);
			break;
		case opcode::f64_convert_u_i64:
#ifdef PRINT_DISASSEMBLY
			printf("i64.convert_u/i64\n");
#endif
			Convert(value_type::f64, value_type::i64, false /*fSigned*/);
			break;
		case opcode::f64_promote_f32:
#ifdef PRINT_DISASSEMBLY
			printf("f64.promote/f32\n");
#endif
			Convert(value_type::f64, value_type::f32, true);
			break;
		case opcode::f32_demote_f64:
#ifdef PRINT_DISASSEMBLY
			printf("f32.demote/f64\n");
#endif
			Convert(value_type::f32, value_type::f64, true);
			break;

		case opcode::i32_reinterpret_f32:
		case opcode::i64_reinterpret_f64:
		case opcode::f32_reinterpret_i32:
		case opcode::f64_reinterpret_i64:
			break; //nop

		case opcode::end:
#ifdef PRINT_DISASSEMBLY
			printf("end\n");
#endif
			if (stackBlockTypeAddr.size() > 1)
			{
				LeaveBlock(stackBlockTypeAddr.back().first != value_type::empty_block);	// don't "leave" the function
			}
			else
			{
				Verify(stackBlockTypeAddr.back().first == value_type::none);
				LeaveBlock(true);
			}
			// Jump targets are after the LeaveBlock because the branch already performs the work (TODO: Maybe not do that?)
			for (int32_t *poffsetFix : stackVecFixupsRelative.back())
			{
				*poffsetFix = numeric_cast<int32_t>(m_pexecPlaneCur - (reinterpret_cast<uint8_t*>(poffsetFix) + sizeof(*poffsetFix)));
			}
			for (void **pp : stackVecFixupsAbsolute.back())
			{
				*pp = m_pexecPlaneCur;
			}

			stackBlockTypeAddr.pop_back();
			stackVecFixupsRelative.pop_back();
			stackVecFixupsAbsolute.pop_back();
			break;

		case opcode::current_memory:
		{
			uint8_t reserved = safe_read_buffer<uint8_t>(&pop, &cb);
#ifdef PRINT_DISASSEMBLY
			printf("current_memory\n");
#endif
			PushC32(0);						// re-use grow_memory, just give a delta of 0
			CallAsmOp(m_pfnGrowMemoryOp);
			break;
		}
		case opcode::grow_memory:
		{
			uint8_t reserved = safe_read_buffer<uint8_t>(&pop, &cb);
#ifdef PRINT_DISASSEMBLY
			printf("grow_memory\n");
#endif
			CallAsmOp(m_pfnGrowMemoryOp);
			break;
		}
			
		default:
			throw RuntimeException("Invalid opcode");

		}
	}
	FnEpilogue(m_pctxt->m_vecfn_types[itype]->fHasReturnValue);

	for (uint32_t ifnCompile : vecifnCompile)
	{
		void *&pfn = reinterpret_cast<void**>(m_pexecPlane)[ifnCompile];
		if (pfn == nullptr)
		{
			CompileFn(ifnCompile);
		}
	}
#ifdef PRINT_DISASSEMBLY
	printf("\n\n");
#endif
}

extern "C" uint64_t ExternCallFnASM(ExecutionControlBlock *pctl);


void JitWriter::ProtectForRuntime()
{
	layer::ProtectRange(*m_spapbExecPlane, m_pcodeStart, m_pexecPlaneCur - m_pcodeStart, layer::PAGE_PROTECTION::ReadExecute);
}

void JitWriter::UnprotectRuntime()
{
	layer::ProtectRange(*m_spapbExecPlane, m_pcodeStart, m_pexecPlaneCur - m_pcodeStart, layer::PAGE_PROTECTION::ReadWrite);
}

ExpressionService::Variant JitWriter::ExternCallFn(uint32_t ifn, void *pvAddr, ExpressionService::Variant *rgargs, uint32_t cargs)
{
	uint64_t retV;
	size_t itype = m_pctxt->m_vecfn_entries.at(ifn);
	auto ptype = m_pctxt->m_vecfn_types[itype].get();
	void *&pfn = reinterpret_cast<void**>(m_pexecPlane)[ifn];
	if (pfn == nullptr)
	{
		CompileFn(ifn);
	}
	
	m_vecoperand.resize(4096 * 100);
	m_veclocals.resize(4096 * 100);
	Verify(pfn != nullptr);
	if (m_pheap == nullptr)
	{
		// Reserve 8GB of memory for our heap plane, this is 2^33 because effective addresses can compute to 33 bits (even though we actually truncate to 32 we reserve the max to prevent security flaws if we truncate incorrectly)
		const size_t cbAlloc = 0x200000000;
		m_spapbHeap = layer::ReservePages(nullptr, cbAlloc);
		layer::ProtectRange(*m_spapbHeap, m_spapbHeap->PvBaseAddr(), cbAlloc, layer::PAGE_PROTECTION::ReadWrite);
		m_pheap = m_spapbHeap->PvBaseAddr();
		memcpy(m_pheap, m_pctxt->m_vecmem.data(), m_pctxt->m_vecmem.size());
	}

	// Process Arguments
	for (uint32_t iarg = 0; iarg < cargs; ++iarg)
	{
		m_veclocals[iarg] = rgargs[iarg].val;
	}

	ExecutionControlBlock ectl;
	ectl.pjitWriter = this;
	ectl.pfnEntry = pfn;
	ectl.operandStack = m_vecoperand.data();
	ectl.localsStack = m_veclocals.data();
	if (m_pctxt->m_vecmem_types.size() > 0)
	{
		ectl.cbHeap = m_pctxt->m_vecmem_types[0].initial_size * (64 * 1024);
	}
	else
	{
		ectl.cbHeap = 0;
	}
	ectl.memoryBase = m_pheap;
	ectl.cFnIndirect = m_pctxt->m_vecIndirectFnTable.size();
	ectl.rgfnIndirect = m_pctxt->m_vecIndirectFnTable.data();
	ectl.rgFnTypeIndicies = m_pctxt->m_vecfn_entries.data();
	ectl.cFnTypeIndicies = m_pctxt->m_vecfn_entries.size();
	ectl.rgFnPtrs = (void*)m_pexecPlane;
	ectl.cFnPtrs = m_cfn;
	
	ProtectForRuntime();
	retV = ExternCallFnASM(&ectl);
	UnprotectRuntime();
	Verify(retV);
	Verify(ectl.operandStack >= m_vecoperand.data());
	Verify(ectl.localsStack >= m_veclocals.data());
	if (ectl.cbHeap > 0)
		m_pctxt->m_vecmem_types[0].initial_size = numeric_cast<uint32_t>(ectl.cbHeap / (64 * 1024));

	ExpressionService::Variant varRet;
	
	varRet.type = ptype->fHasReturnValue ? ptype->return_type : value_type::none;
	if (ptype->fHasReturnValue)
		varRet.val = ectl.retvalue;
	return varRet;
}

extern "C" void CompileFn(ExecutionControlBlock *pectl, uint32_t ifn)
{
	pectl->pjitWriter->UnprotectRuntime();
	pectl->pjitWriter->CompileFn(ifn);
	pectl->pjitWriter->ProtectForRuntime();
}

uint32_t JitWriter::GrowMemory(ExecutionControlBlock *pectl, uint32_t cpages)
{
	size_t cb = size_t(cpages) * 64 * 1024;	// convert to bytes
	size_t cbMax = 0x100000000;
	if (m_pctxt->m_vecmem_types.size() > 0 && m_pctxt->m_vecmem_types[0].fMaxSet)
	{
		cbMax = m_pctxt->m_vecmem_types[0].maximum_size * (64 * 1024ULL);
	}
	if ((pectl->cbHeap + cb) > cbMax)
		return -1;
	Verify(pectl->cbHeap + cb < 0x100000000);
	uint32_t cbRet = (uint32_t)(pectl->cbHeap / (64 * 1024));
	pectl->cbHeap += cb;
	return cbRet;
}
extern "C" uint32_t GrowMemory(ExecutionControlBlock *pectl, uint32_t cpages)
{
	return pectl->pjitWriter->GrowMemory(pectl, cpages);
}
