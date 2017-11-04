#include "stdafx.h"
#include "ExpressionService.h"
#include "safe_access.h"
#include "Exceptions.h"

size_t ExpressionService::CbEatExpression(const uint8_t *rgb, size_t cb, _Out_ Variant *pvariantOut)
{
	const size_t cbStart = cb;
	opcode op = safe_read_buffer<opcode>(&rgb, &cb);
	switch (op)
	{
	case opcode::i32_const:
		pvariantOut->type = value_type::i32;
		pvariantOut->val = safe_read_buffer<varint32>(&rgb, &cb);
		break;
	case opcode::i64_const:
		pvariantOut->type = value_type::i64;
		pvariantOut->val = safe_read_buffer<varint64>(&rgb, &cb);
		break;

	case opcode::f32_const:
		pvariantOut->type = value_type::f32;
		pvariantOut->val = 0;
		*reinterpret_cast<float*>(&pvariantOut->val) = safe_read_buffer<float>(&rgb, &cb);
		break;

	case opcode::f64_const:
		pvariantOut->type = value_type::f64;
		*reinterpret_cast<double*>(&pvariantOut->val) = safe_read_buffer<double>(&rgb, &cb);
		break;

	default:
		Verify(false);
	}

	op = safe_read_buffer<opcode>(&rgb, &cb);
	Verify(op == opcode::end);

	return cbStart - cb;
}

value_type ParseTypeString(const std::string &strType)
{
	if (strType == "i32")
		return value_type::i32;
	if (strType == "i64")
		return value_type::i64;
	if (strType == "f32")
		return value_type::f32;
	if (strType == "f64")
		return value_type::f64;
	Verify(false);
	return value_type::none;
}

size_t ExpressionService::CchEatExpression(const char *sz, size_t cch, _Out_ Variant *pvalOut)
{
	// eat expressions such as (i32.const 123)
	const char *pchCur = sz;
	const char *pchMax = sz + cch;
	
	// OpenParen -> type -> ".const" -> val -> CloseParen
	int mode = 0;
	std::string strType;
	std::string strConst;
	std::string strVal;
	while (pchCur < pchMax)
	{
		if (isspace(*pchCur))
		{
			Verify(mode != 1 || strType.size() == 0);
			if (mode == 2 && strConst.size() > 0)
			{
				Verify(strConst == "const");
				++mode;
			}
			Verify(mode != 3 || strVal.size() == 0);
			++pchCur;
			continue;
		}

		switch (mode)
		{
		default:
			Verify(false);
			break;

		case 0:
			Verify(*pchCur == '(');
			++mode;
			break;

		case 1:
			if (*pchCur == '.')
			{
				Verify(strType.size() > 0);
				pvalOut->type = ParseTypeString(strType);
				++mode;
			}
			else
			{
				strType.append(pchCur, pchCur + 1);
			}
			break;

		case 2:
			strConst.append(pchCur, pchCur + 1);
			break;

		case 3:
			if (*pchCur == ')')
			{
				Verify(strVal.size() > 0);
				switch (pvalOut->type)
				{
				case value_type::i32:
				case value_type::i64:
				{
					int base = 10;
					if (strVal.size() > 2 && strVal[0] == '0' && strVal[1] == 'x')
					{
						// Hex
						base = 16;
						strVal = std::string(strVal.begin() + 2, strVal.end());
					}
					pvalOut->val = static_cast<uint64_t>(std::stoull(strVal, nullptr, base));
					if (pvalOut->type == value_type::i32)
					{
						pvalOut->val = (uint32_t)pvalOut->val;
					}
					break;
				}
				case value_type::f32:
				{
					float valT = std::stof(strVal);
					pvalOut->val = *reinterpret_cast<uint32_t*>(&valT);
					break;
				}
				case value_type::f64:
				{
					double valT = std::stod(strVal);
					pvalOut->val = *reinterpret_cast<int64_t*>(&valT);
					break;
				}
				default:
					Verify(false);
				}
				++mode;
				continue;	// don't increment pch
			}
			strVal.append(pchCur, pchCur + 1);
			break;

		case 4:
			Verify(*pchCur == ')');
			return (pchCur + 1) - sz;
		}

		++pchCur;
	}
	Verify(false);
	abort();
}