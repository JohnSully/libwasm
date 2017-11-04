#pragma once

#include "wasm_types.h"
#include <inttypes.h>

typedef uint64_t(*FnBuiltinPtr)(uint64_t *pvArgs, uint8_t *pvMemBase);
struct BuiltinExport
{
	const char *szName;
	value_type retT;
	std::vector<value_type> vecarg;
	FnBuiltinPtr pfn;
};

extern BuiltinExport BuiltinMap[];

bool FEqualProto(const BuiltinExport &builtinexport, const FunctionTypeEntry &fnt);
int IBuiltinFromName(const std::string &strName);