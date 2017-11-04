#pragma once
#include "wasm_types.h"

template<typename T>
void fread_struct(T *dst, FILE *pf, size_t cstructs = 1)
{
	if (cstructs == 0)
		return;
	size_t celem = 0;
	do
	{
		size_t creadT = fread(dst + celem, sizeof(T), cstructs, pf);
		if (creadT == 0)
			throw 0;
		celem += creadT;
	} while (celem < cstructs);
}

template<>
void fread_struct(varuint32 *dst, FILE *pf, size_t cstructs);

template<typename T>
T safe_read_buffer(const uint8_t ** prgb, size_t *pcb)
{
	if (*pcb < sizeof(T))
		throw 0;
	const T *tr = reinterpret_cast<const T*>(*prgb);
	*prgb += sizeof(T);
	*pcb -= sizeof(T);
	return *tr;
}

template<>
varuint32 safe_read_buffer(const uint8_t **prgb, size_t *pcb);

template<>
varuint64 safe_read_buffer(const uint8_t **prgb, size_t *pcb);

template<>
varint32 safe_read_buffer(const uint8_t **prgb, size_t *pcb);

template<>
varint64 safe_read_buffer(const uint8_t **prgb, size_t *pcb);

template<>
std::string safe_read_buffer(const uint8_t **prgb, size_t *pcb);

template<typename T> void safe_copy_buffer(T *rgdst, size_t celem, const uint8_t **prgb, size_t *pcb)
{
	if (*pcb < (sizeof(T)*celem))
		throw 0;
	memcpy(rgdst, *prgb, sizeof(T)*celem);
	*prgb += sizeof(T)*celem;
	*pcb -= sizeof(T)*celem;
}