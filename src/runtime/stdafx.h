// stdafx.h : include file for standard system include files,
// or project specific include files that are used frequently, but
// are changed infrequently
//

#pragma once

#define _CRT_SECURE_NO_WARNINGS 1
#define NOMINMAX 1

#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include <string>
#include <vector>
#include <memory>
#include <stack>
#include <tuple>
#include <cstring>
#include <cstddef>
#include <algorithm>

#ifndef _MSC_VER
template <typename T, std::size_t N>
constexpr std::size_t _countof(T const (&)[N]) noexcept
{
	return N;
}
#define UNREFERENCED_PARAMETER(param) do { (void)(param); } while(0)
#endif
