// stdafx.h : include file for standard system include files,
// or project specific include files that are used frequently, but
// are changed infrequently
//

#pragma once

#define _CRT_SECURE_NO_WARNINGS 1
#define NOMINMAX 1
#ifdef _MSC_VER
#include "targetver.h"
#include <tchar.h>
#else
#include "sal_stubs.h"
#endif

#ifdef _MSC_VER
#define EXPORT __declspec(dllexport)
#else
#define EXPORT
#endif


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
#include "layer/layer.h"
#include "../compiler_bridges.h"
