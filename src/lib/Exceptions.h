#pragma once
#include <string>
struct Exception
{
	Exception(const std::string &strErr)
		: strErr(strErr)
	{}
	std::string strErr;
};

struct RuntimeException : public Exception
{
	RuntimeException(const std::string &strErr)
		: Exception(strErr)
	{}
};

static void Verify(bool fVerify, const char *sz = "Validation Failure")
{
	if (!fVerify)
		throw Exception(sz);
}