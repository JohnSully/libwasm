#pragma once

#include "wasm_types.h"
#include "Exceptions.h"
#include "numeric_cast.h"
#include "ExpressionService.h"

extern "C" void CompileFn(struct ExecutionControlBlock *pectl, uint32_t ifn);
class JitWriter
{
	friend void CompileFn(ExecutionControlBlock *pectl, uint32_t ifn);
public:
	JitWriter(class WasmContext *pctxt, size_t cfn, size_t cglbls);
	~JitWriter();

	void CompileFn(uint32_t ifn);

	ExpressionService::Variant ExternCallFn(uint32_t ifn, void *pvAddrMem, ExpressionService::Variant *rgargs, uint32_t cargs);

	// Psuedo private callbacks from ASM
	uint64_t CReentryFn(int ifn, uint64_t *pvArgs, uint8_t *pvMemBase, ExecutionControlBlock *pecb);
	uint32_t GrowMemory(ExecutionControlBlock *pectl, uint32_t cpages);
private:
	void SafePushCode(const void *pv, size_t cb);
	template<typename T, size_t size>
	size_t GetArrLength(T(&)[size]) { return size; }

	template<typename T>
	void SafePushCode(T &rgcode, std::true_type) 
	{ 
		SafePushCode(rgcode, GetArrLength(rgcode) * sizeof(*rgcode));
	}

	template<typename T>
	void SafePushCode(T &val, std::false_type)
	{
		SafePushCode(&val, sizeof(val));
	}

	template<typename T>
	void SafePushCode(T &&val)
	{
		SafePushCode<T>(val, std::is_array<T>());
	}

	enum class CompareType
	{
		LessThan,
		LessThanEqual,
		Equal,
		NotEqual,
		GreaterThanEqual,
		GreaterThan,
	};
	enum class LogicOperation
	{
		And,
		Or,
		Xor,
		ShiftLeft,
		ShiftRight,
		ShiftRightUnsigned,
		RotateLeft,
		RotateRight,
	};
	enum class ArithmeticOperation
	{
		Add,
		Sub,
		Multiply,
		Divide,
	};

	int32_t RelAddrPfnVector(uint32_t ifn, uint32_t opSize) const
	{
		return numeric_cast<int32_t>((m_pexecPlane + (sizeof(void*)*ifn)) - (m_pexecPlaneCur + opSize));
	}

	// Common code sequences (does not leave machine in valid state)
	void _PushExpandStack();
	void _PopContractStack();
	void _PopSecondParam(bool fSwapParams = false);
	void _SetDbgReg(uint32_t opcode);

	// common operations (does leave machine in valid state)
	void LoadMem(uint32_t offset, bool f64Dst /* else 32 */, uint32_t cbSrc, bool fSignExtend);
	void StoreMem(uint32_t offset, uint32_t cbDst);

	void Sub32();
	void Add32();
	void Mul32();
	void Add64();
	void Sub64();
	void Mul64();
	void Div(bool fSigned, bool fModulo, bool f64);
	void Popcnt32();
	void Popcnt64();
	void PushC32(uint32_t c);
	void PushC64(uint64_t c);
	void PushF32(float c);
	void PushF64(double c);
	void Convert(value_type typeDst, value_type typeSrc, bool fSigned);
	void SetLocal(uint32_t idx, bool fPop);
	void GetLocal(uint32_t idx);
	void GetGlobal(uint32_t idx);
	void SetGlobal(uint32_t idx);
	void CountTrailingZeros(bool f64);
	void CountLeadingZeros(bool f64);
	void Select();
	void Compare(CompareType type, bool fSigned, bool f64);
	void FloatCompare(CompareType type);
	void DoubleCompare(CompareType type);
	void Eqz32();
	void Eqz64();
	void CallAsmOp(void **pfn);
	void LogicOp(LogicOperation op);
	void LogicOp64(LogicOperation op);
	void FloatArithmetic(ArithmeticOperation op, bool fDouble);
	int32_t *JumpNIf(void *addr);	// returns a pointer to the offset encoded in the instruction for later adjustment
	int32_t *Jump(void *addr);
	void CallIfn(uint32_t ifn, uint32_t clocalsCaller, uint32_t cargsCallee, bool fReturnValue, bool fIndirect);
	void FnEpilogue(bool fRetVal);
	void FnPrologue(uint32_t clocals, uint32_t cargs);
	void BranchTableParse(const uint8_t **ppoperand, size_t *pcbOperand, const std::vector<std::pair<value_type, void*>> &stackBlockTypeAddr, std::vector<std::vector<int32_t*>> &stackVecFixups, std::vector<std::vector<void**>> &stackVecFixupsAbsolute);
	void ExtendSigned32_64();
	void FloatNeg(bool fDouble);

	void Ud2();

	void EnterBlock();
	int32_t *EnterIF();
	void LeaveBlock(bool fHasReturn);

	void ProtectForRuntime();
	void UnprotectRuntime();

	class WasmContext *m_pctxt = nullptr;	// Parent
	uint8_t *m_pexecPlane = nullptr;
	uint8_t *m_pcodeStart = nullptr;
	uint8_t *m_pexecPlaneCur = nullptr;
	uint8_t *m_pexecPlaneMax = nullptr;
	void **m_pfnCallIndirectShim = nullptr;
	void **m_pfnBranchTable = nullptr;
	void **m_pfnU64ToF32 = nullptr;
	void **m_pfnU64ToF64 = nullptr;
	void **m_pfnGrowMemoryOp = nullptr;
	void **m_pfnF32ToU64Trunc = nullptr;
	void **m_pfnF64ToU64Trunc = nullptr;
	uint64_t *m_pGlobalsStart = nullptr;
	void *m_pheap = nullptr;
	size_t m_cfn;

	std::vector<uint64_t> m_vecoperand;
	std::vector<uint64_t> m_veclocals;
	std::unique_ptr<layer::AllocatedPageBlock> m_spapbExecPlane;
	std::unique_ptr<layer::AllocatedPageBlock> m_spapbHeap;
};
