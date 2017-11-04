#pragma once

struct ExecutionControlBlock
{
	class JitWriter *pjitWriter;
	void *pfnEntry;
	void *operandStack;
	void *localsStack;
	uint64_t cbHeap;
	void *memoryBase;

	uint64_t cFnIndirect;
	uint32_t *rgfnIndirect;
	uint32_t *rgFnTypeIndicies;
	uint64_t cFnTypeIndicies;
	void *rgFnPtrs;
	uint64_t cFnPtrs;

	// Values set by the executing code
	void *stackrestore;
	uint64_t retvalue;
};