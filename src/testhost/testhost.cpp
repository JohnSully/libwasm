#include <stdafx.h>
#include <Exceptions.h>
#include <numeric_cast.h>
#include <WasmContext.h>
#include <ExpressionService.h>
#include <assert.h>
#ifdef _MSC_VER
#include 	<process.h>
#define NOMINMAX 1
#include <Windows.h>
#else
template<size_t ARRAY_SIZE>
void strcat_s(char (&szDst)[ARRAY_SIZE], const char *szSrc)
{
	Verify(ARRAY_SIZE >= (strlen(szSrc) + strlen(szDst) + 1));
	strcat(szDst, szSrc);
}
template<size_t ARRAY_SIZE>
void strcpy_s(char (&szDst)[ARRAY_SIZE], const char *szSrc)
{
	Verify(ARRAY_SIZE >= (strlen(szSrc) + strlen(szDst) + 1));
	strcpy(szDst, szSrc);
}
#define MAX_PATH 1024
#endif
#include <algorithm>

enum class ParseMode
{
	Whitespace,
	Comment,
	Quote,
	Escape,
	Command,
};

std::unique_ptr<WasmContext> g_spctxtLast;
ExpressionService::Variant g_variantLastExec;
ExpressionService::Variant g_variantExpectedReturn;

const char *rgszUnsupported[] = {
	"assert_trap",
	"assert_invalid",
	"assert_malformed",
	"assert_unlinkable",
	"assert_exhaustion",
	"assert_return_canonical_nan",
	"assert_return_arithmetic_nan",
};

bool FUnsupportedCommand(const std::string &str)
{
	for (const char *sz : rgszUnsupported)
	{
		if (str == sz)
			return true;
	}
	return false;
}

#ifdef _MSC_VER
int RunProgram(const char *szProgram, const char *szArgs)
{
	SHELLEXECUTEINFOA shellexeca = { 0 };
	shellexeca.cbSize = sizeof(SHELLEXECUTEINFOA);
	shellexeca.fMask = SEE_MASK_FLAG_NO_UI | SEE_MASK_NOCLOSEPROCESS;
	shellexeca.lpVerb = "open";
	shellexeca.lpFile = szProgram;
	shellexeca.lpParameters = szArgs;
	shellexeca.nShow = SW_SHOW;

	ShellExecuteExA(&shellexeca);
	assert(shellexeca.hProcess != nullptr);
	WaitForSingleObject(shellexeca.hProcess, 10000);
	DWORD retV;
	GetExitCodeProcess(shellexeca.hProcess, &retV);
	return (int)retV;
}
#else
int RunProgram(const char *szProgram, const char *szArgs)
{
	size_t cch = strlen(szProgram) + strlen(szArgs) + 1;
	cch++;	// null terminator
	char *szExec = (char*)malloc(cch);
	strcpy(szExec, szProgram);
	strcat(szExec, " ");
	strcat(szExec, szArgs);
	int retV = system(szExec);
	free(szExec);
	return retV;
}
#endif

void ProcessInvoke(const char *rgch, size_t cch)
{
	size_t ichFnStart = 0;
	assert(cch > 0);
	bool fEscaped = false;
	while (rgch[ichFnStart] != '"' || fEscaped)
	{
		fEscaped = rgch[ichFnStart] == '\\';
		assert((ichFnStart + 1) < cch);
		++ichFnStart;
	}
	fEscaped = false;
	++ichFnStart;	// after the quote
	size_t ichFnEnd = ichFnStart;
	assert(ichFnEnd < cch);
	while (rgch[ichFnEnd] != '"' || fEscaped)
	{
		fEscaped = rgch[ichFnEnd] == '\\';
		assert((ichFnEnd + 1) < cch);
		++ichFnEnd;
	}

	std::string strFnExec(rgch + ichFnStart, rgch + ichFnEnd);

	// Gather Arguments
	std::vector<ExpressionService::Variant> vecargs;
	size_t ichArgStart = ichFnEnd + 1;
	while (ichArgStart < cch)
	{
		if (isspace(rgch[ichArgStart]))
		{
			++ichArgStart;
			continue;
		}
		if (rgch[ichArgStart] == '(')
		{
			vecargs.push_back(ExpressionService::Variant());
			ichArgStart += ExpressionService::CchEatExpression(rgch + ichArgStart, cch - ichArgStart, &vecargs.back());
			continue;
		}
		else
		{
			Verify(rgch[ichArgStart] == ')');
			break;
		}
		Verify(false);	// should never get here
	}

	printf("Invoke: %s\n", strFnExec.c_str());
	g_variantLastExec = g_spctxtLast->CallFunction(strFnExec.c_str(), vecargs.data(), numeric_cast<uint32_t>(vecargs.size()));
}

void ProcessCommand(const std::string &str, FILE *pf, off_t offsetStart, off_t offsetEnd)
{
	char rgchT[1024];
	off_t offsetCur = ftell(pf);
	fseek(pf, offsetStart, SEEK_SET);

	if (str == "module")
	{
		// compile the module and set the current WasmContext

		char szPathWast[MAX_PATH];
		char szPathWasm[MAX_PATH];
	
#ifdef _MSC_VER	
		GetTempPathA(MAX_PATH, szPathWast);
#else
		strcpy_s(szPathWast, "/tmp/");
#endif
		strcpy(szPathWasm, szPathWast);
		strcat_s(szPathWast, "temp.wast");
		strcat_s(szPathWasm, "temp.wasm");

		// copy the module portion
		size_t cbWast = offsetEnd - offsetStart;
		FILE *pfWast = fopen(szPathWast, "w+");
		if (pfWast == nullptr)
			throw "failed to open temp file";

		for (size_t cbWritten = 0; cbWritten < cbWast; )
		{
			size_t cb = std::min<size_t>(1024, cbWast - cbWritten);
			size_t cbT = fread(rgchT, 1, cb, pf);
			if (cbT == 0)
				throw "failed to read wast";
			size_t cbWrote = fwrite(rgchT, 1, cbT, pfWast);
			assert(cbWrote == cbT);
			cbWritten += cbT;
		}
		fclose(pfWast);

		// compile the module portion
		char szParams[1024] = { '\0' };
		strcat_s(szParams, szPathWast);
		strcat_s(szParams, " --no-check -o ");
		strcat_s(szParams, szPathWasm);
		int res = RunProgram("wat2wasm", szParams);
		if (res == EXIT_SUCCESS)
		{
			g_spctxtLast = std::unique_ptr<WasmContext>(new WasmContext);
			FILE *pfWasm = fopen(szPathWasm, "rb");
			try
			{
				g_spctxtLast->LoadModule(pfWasm);
			}
			catch (Exception)
			{
				g_spctxtLast = nullptr;
			}
			fclose(pfWasm);
		}
		else
		{
			g_spctxtLast = nullptr;
		}
	}
	else if (str == "invoke")
	{
		std::vector<char> vecch;
		vecch.resize(offsetEnd - offsetStart);
		fread(vecch.data(), 1, vecch.size(), pf);
		ProcessInvoke(vecch.data(), vecch.size());
		g_variantExpectedReturn.type = value_type::none;
		g_variantExpectedReturn.val = 0;
	}
	else if (str == "assert_return")
	{
		Verify(g_variantExpectedReturn == g_variantLastExec);
	}
	else if (str == "assert_trap")
	{
		//assert(false);
	}
	else if (FUnsupportedCommand(str))
	{
		//Unsupported tests
	}
	else
	{
		// If its not a verb we expect we are assuming its a return value expression
		std::vector<char> vecch;
		vecch.resize(offsetEnd - offsetStart);
		fread(vecch.data(), 1, vecch.size(), pf);
		ExpressionService::CchEatExpression(vecch.data(), vecch.size(), &g_variantExpectedReturn);
	}

	fseek(pf, offsetCur, SEEK_SET);
}

int main(int argc, char *argv[])
{
	if (argc != 2)
	{
		fprintf(stderr, "Expected test file.\n");
		return EXIT_FAILURE;
	}
	FILE *pf = fopen(argv[1], "rb");
	if (pf == nullptr)
	{
		fprintf(stderr, "Failed to open test file.\n");
		return EXIT_FAILURE;
	}

	char rgch[1024];
	size_t cch;
	std::stack<ParseMode> stackMode;
	stackMode.push(ParseMode::Whitespace);
	bool fEscapeLast = false;
	bool fCommentLast = false;
	int cblock = 0;
	std::stack<off_t> stackoffsetBlockStart;
	bool fNestCmd = false;

	std::stack<std::string> stackstrCmd;
	while ((cch = fread(rgch, 1, 1024, pf)) > 0)
	{
		const char *pch = rgch;
		const char *pchMax = rgch + cch;

		while (pch < pchMax)
		{
			ParseMode mode = stackMode.top();
			bool fCommentLastT = fCommentLast;
			fCommentLast = false;

			if (mode == ParseMode::Command)
			{
				if ((*pch >= 'a' && *pch <= 'z') || (*pch >= 'A' && *pch <= 'Z') || (*pch >= '0' && *pch <= '9') || *pch == '_')
				{
					stackstrCmd.top().append(pch, pch + 1);
				}
				else
				{
					fNestCmd = stackstrCmd.top() != "module" && !FUnsupportedCommand(stackstrCmd.top());
					stackMode.pop();
					mode = stackMode.top();
				}
			}

			if (mode == ParseMode::Quote)
			{
				if (*pch == '"' && !fEscapeLast)
					stackMode.pop();
			}
			else if (mode == ParseMode::Comment)
			{
				if (*pch == '\n' || (*pch == ';' && !fCommentLastT))
				{
					stackMode.pop();
					mode = stackMode.top();
				}
			}
			else if (mode == ParseMode::Whitespace)
			{
				switch (*pch)
				{
				case '"':
					stackMode.push(ParseMode::Quote);
					break;

				case ';':
					stackMode.push(ParseMode::Comment);
					fCommentLast = true;
					break;

				case '(':
					if (cblock == 0 || fNestCmd)
					{
						stackstrCmd.push(std::string());
						stackoffsetBlockStart.push(numeric_cast<off_t, false /*off_t varies size */>(ftell(pf) - (pchMax - pch)));
						stackMode.push(ParseMode::Command);
					}
					++cblock;
					break;

				case ')':
					if (cblock == 1 || fNestCmd)
					{
						off_t offsetCur = numeric_cast<off_t, false /*off_t varies size */>(ftell(pf) - (pchMax - (pch + 1)));
						ProcessCommand(stackstrCmd.top(), pf, stackoffsetBlockStart.top(), offsetCur);
						stackstrCmd.pop();
						stackoffsetBlockStart.pop();
						if (cblock == 1)
							fNestCmd = false;
					}
					--cblock;
					break;
				}
			}
			fEscapeLast = *pch == '\\';
			++pch;
		}
	}
	assert(stackstrCmd.empty());
	assert(cblock == 0);
	stackMode.pop();
	assert(stackMode.empty());
	fclose(pf);

	return EXIT_SUCCESS;
}
