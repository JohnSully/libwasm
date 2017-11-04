// webasmRT.cpp : Defines the entry point for the console application.
//

#include "stdafx.h"
#include <inttypes.h>
#include <WasmContext.h>

int main(int argc, char *argv[])
{
	if (argc < 2)
	{
		fprintf(stderr, "Usage: module.wasm\n");
		return EXIT_FAILURE;
	}
	FILE *pf = fopen(argv[1], "rb");
	if (pf == nullptr)
	{
		perror("Could not open wasm module");
		return EXIT_FAILURE;
	}

	WasmContext ctxt;
	ctxt.LoadModule(pf);
	fclose(pf);
	
	ctxt.CallFunction("main");

    return EXIT_SUCCESS;
}

