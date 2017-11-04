#include "stdafx.h"
#include "safe_access.h"
#include "Exceptions.h"

template<>
void fread_struct(varuint32 *dst, FILE *pf, size_t cstructs)
{
	for (size_t istruct = 0; istruct < cstructs; ++istruct)
	{
		uint8_t val;
		varuint32 &dstCur = dst[istruct];
		dstCur = 0;
		uint32_t shift = 0;
		do
		{
			fread_struct(&val, pf);
			dstCur |= (val & 0x7F) << shift;
			shift += 7;
		} while (val & 0x80);
	}
}

template<>
varuint32 safe_read_buffer(const uint8_t **prgb, size_t *pcb)
{
	uint32_t ret = 0;
	uint32_t shift = 0;
	for (int ib = 0; ib < 5 && *pcb > 0; ++ib)
	{
		ret |= (**prgb & 0x7F) << shift;
		shift += 7;
		bool fContinue = !!(**prgb & 0x80);
		(*prgb)++;
		(*pcb)--;
		if (!fContinue)
			break;
	}
	return ret;
}

template<>
varuint64 safe_read_buffer(const uint8_t **prgb, size_t *pcb)
{
	uint64_t ret = 0;
	uint32_t shift = 0;
	for (int ib = 0; ib < 10 && *pcb > 0; ++ib)
	{
		ret |= static_cast<uint64_t>(**prgb & 0x7F) << shift;
		shift += 7;
		bool fContinue = !!(**prgb & 0x80);
		(*prgb)++;
		(*pcb)--;
		if (!fContinue)
			break;
	}
	return ret;
}

template<>
varint32 safe_read_buffer(const uint8_t **prgb, size_t *pcb)
{
	int32_t ret = 0;
	uint32_t shift = 0;
	uint8_t byteLast = 0;
	for (int ib = 0; ib < 5 && *pcb > 0; ++ib)
	{
		byteLast = **prgb;
		ret |= (byteLast & 0x7F) << shift;
		shift += 7;
		bool fContinue = !!(**prgb & 0x80);
		(*prgb)++;
		(*pcb)--;
		if (!fContinue)
			break;
	}

	/* sign bit of byte is second high order bit (0x40) */
	if ((shift < 32) && (byteLast & 0x40))
		ret |= (~0 << shift);		// sign extend
	return ret;
}

template<>
varint64 safe_read_buffer(const uint8_t **prgb, size_t *pcb)
{
	int64_t ret = 0;
	uint64_t shift = 0;
	uint8_t byteLast = 0;
	for (int ib = 0; ib < 10 && *pcb > 0; ++ib)
	{
		byteLast = **prgb;
		ret |= static_cast<int64_t>(byteLast & 0x7F) << shift;
		shift += 7;
		bool fContinue = !!(**prgb & 0x80);
		(*prgb)++;
		(*pcb)--;
		if (!fContinue)
			break;
	}

	/* sign bit of byte is second high order bit (0x40) */
	if ((shift < 64) && (byteLast & 0x40))
		ret |= (~0 << shift);		// sign extend
	return ret;
}


template<>
std::string safe_read_buffer(const uint8_t **prgb, size_t *pcb)
{
	varuint32 cch = safe_read_buffer<varuint32>(prgb, pcb);
	std::string str;
	if (*pcb < cch)
		throw 0;
	str.assign(*prgb, *prgb + cch);
	Verify(strlen(str.c_str()) <= cch);
	*prgb += cch;
	*pcb -= cch;
	return str;
}