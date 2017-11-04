#pragma once
#include "wasm_types.h"

// The expression service executes 1-off expressions as required by the loader.  This is not intended to supplement behavior of the JIT engine
class ExpressionService
{
public:
	struct Variant
	{
		bool operator==(const Variant &other)
		{
			return (type == other.type) && (val == other.val);
		}
		uint64_t val = 0;
		value_type type = value_type::none;
	};

	static size_t CbEatExpression(const uint8_t *rgb, size_t cb, Variant *pvalOut);	// consume an expression from a byte buffer and inform the caller how long it was
	static size_t CchEatExpression(const char *sz, size_t cch, Variant *pvalOut);
};
