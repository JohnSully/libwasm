#include "stdafx.h"
#include "WasmContext.h"
#include "wasm_types.h"
#include "safe_access.h"
#include "Exceptions.h"
#include "ExpressionService.h"
#include "BuiltinFunctions.h"
#include "JitWriter.h"

WasmContext::WasmContext() {}
WasmContext::~WasmContext() {}

void WasmContext::load_fn_type(const uint8_t **prgbPayload, size_t *pcbData)
{
	value_type form = safe_read_buffer<value_type>(prgbPayload, pcbData);
	Verify(form == value_type::func);

	varuint32 paramCount = safe_read_buffer<varuint32>(prgbPayload, pcbData);

	auto spfne = FunctionTypeEntry::CreateFunctionEntry(paramCount);

	for (uint32_t iparam = 0; iparam < paramCount; ++iparam)
	{
		spfne->rgparam_type[iparam] = safe_read_buffer<value_type>(prgbPayload, pcbData);
	}

	spfne->fHasReturnValue = safe_read_buffer<uint8_t>(prgbPayload, pcbData) == 1;
	if (spfne->fHasReturnValue)
		spfne->return_type = safe_read_buffer<value_type>(prgbPayload, pcbData);

	m_vecfn_types.emplace_back(std::move(spfne));
}

void WasmContext::load_fn_types(const uint8_t *rgbPayload, size_t cbData)
{
	varuint32 var32cfn = safe_read_buffer<varuint32>(&rgbPayload, &cbData);
	uint32_t cfn = var32cfn;

	while (cfn > 0)
	{
		load_fn_type(&rgbPayload, &cbData);
		cfn--;
	}
	Verify(cbData == 0);
}

void WasmContext::load_fn_decls(const uint8_t *rgbPayload, size_t cbData)
{
	varuint32 var32cfn = safe_read_buffer<varuint32>(&rgbPayload, &cbData);
	uint32_t cfn = var32cfn;
	m_vecfn_entries.reserve(m_vecfn_entries.size() + cfn);
	while (cfn > 0)
	{
		m_vecfn_entries.push_back(safe_read_buffer<varuint32>(&rgbPayload, &cbData));
		Verify(m_vecfn_entries.back() < m_vecfn_types.size());
		--cfn;
	}
	Verify(cbData == 0);
}

resizable_limits load_resizeable_limits(const uint8_t **prgbPayload, size_t *pcbData)
{
	resizable_limits limits;
	limits.fMaxSet = !!(safe_read_buffer<uint8_t>(prgbPayload, pcbData) & 1);
	limits.initial_size = safe_read_buffer<varuint32>(prgbPayload, pcbData);
	if (limits.fMaxSet)
		limits.maximum_size = safe_read_buffer<varuint32>(prgbPayload, pcbData);
	return limits;
}

local_entry load_local_entry(const uint8_t **prgbPayload, size_t *pcbData)
{
	local_entry le;
	le.count = safe_read_buffer<varuint32>(prgbPayload, pcbData);
	le.type = safe_read_buffer<value_type>(prgbPayload, pcbData);
	return le;
}

void WasmContext::load_tables(const uint8_t *rgbPayload, size_t cbData)
{
	varuint32 var32ctbl = safe_read_buffer<varuint32>(&rgbPayload, &cbData);
	uint32_t ctbl = var32ctbl;
	m_vectbl.reserve(ctbl);
	for (uint32_t itbl = 0; itbl < ctbl; ++itbl)
	{
		table_type tbl;
		tbl.type = safe_read_buffer<elem_type>(&rgbPayload, &cbData);
		tbl.limits = load_resizeable_limits(&rgbPayload, &cbData);
		if (itbl == 0)
		{
			Verify(tbl.type == elem_type::anyfunc);
			m_vecIndirectFnTable.resize(tbl.limits.maximum_size);
		}
		else
		{
			Verify(false);
		}
	}
	Verify(cbData == 0);
}

void WasmContext::load_memory(const uint8_t *rgbPayload, size_t cbData)
{
	varuint32 var32cmemt = safe_read_buffer<varuint32>(&rgbPayload, &cbData);
	uint32_t cmemt = var32cmemt;
	m_vecmem_types.reserve(cmemt);
	while (cmemt > 0)
	{
		m_vecmem_types.emplace_back(load_resizeable_limits(&rgbPayload, &cbData));
		--cmemt;
	}
	Verify(cbData == 0);
}

void WasmContext::load_globals(const uint8_t *rgbPayload, size_t cbData)
{
	uint32_t cglobals = safe_read_buffer<varuint32>(&rgbPayload, &cbData);
	while (cglobals > 0)
	{
		value_type type = safe_read_buffer<value_type>(&rgbPayload, &cbData);
		bool fMutable = !!safe_read_buffer<uint8_t>(&rgbPayload, &cbData);

		ExpressionService::Variant variant;
		size_t cbExpr = ExpressionService::CbEatExpression(rgbPayload, cbData, &variant);
		rgbPayload += cbExpr;
		cbData -= cbExpr;
		m_vecglbls.push_back({ variant.val, type, fMutable });
		--cglobals;
	}
	Verify(cbData == 0);
}

void WasmContext::load_exports(const uint8_t *rgbPayload, size_t cbData)
{
	varuint32 var32cexp = safe_read_buffer<varuint32>(&rgbPayload, &cbData);
	uint32_t cexp = var32cexp;
	m_vecexports.reserve(cexp);

	while (cexp > 0)
	{
		export_entry entry;
		entry.strName = safe_read_buffer<std::string>(&rgbPayload, &cbData);
		entry.kind = safe_read_buffer<external_kind>(&rgbPayload, &cbData);
		entry.index = safe_read_buffer<varuint32>(&rgbPayload, &cbData);
		m_vecexports.emplace_back(std::move(entry));
		--cexp;
	}
	Verify(cbData == 0);
}

void WasmContext::load_code(const uint8_t *rgbPayload, size_t cbData)
{
	varuint32 var32cfn = safe_read_buffer<varuint32>(&rgbPayload, &cbData);
	uint32_t cfn = var32cfn;

	while (cfn > 0)
	{
		size_t cbBody = safe_read_buffer<varuint32>(&rgbPayload, &cbData);
		Verify(cbBody <= cbData);
		cbData -= cbBody;
		varuint32 clocal = safe_read_buffer<varuint32>(&rgbPayload, &cbBody);
		auto spfnce = FunctionCodeEntry::CreateFunctionCodeEntry(clocal);
		for (size_t ilocal = 0; ilocal < clocal; ++ilocal)
		{
			spfnce->rglocals[ilocal] = load_local_entry(&rgbPayload, &cbBody);
		}
		spfnce->vecbytecode.resize(cbBody);
		safe_copy_buffer(spfnce->vecbytecode.data(), cbBody, &rgbPayload, &cbBody);
		Verify((opcode)spfnce->vecbytecode.back() == opcode::end);

		m_vecfn_code.emplace_back(std::move(spfnce));
		--cfn;
	}
	Verify(cbData == 0);
}

void WasmContext::load_imports(const uint8_t *rgbPayload, size_t cbData)
{
	varuint32 var32cimport = safe_read_buffer<varuint32>(&rgbPayload, &cbData);
	uint32_t cimport = var32cimport;

	while (cimport > 0)
	{
		uint32_t module_len = safe_read_buffer<varuint32>(&rgbPayload, &cbData);
		std::vector<char> vecrgchModule;
		vecrgchModule.resize(module_len);
		safe_copy_buffer(vecrgchModule.data(), module_len, &rgbPayload, &cbData);
		uint32_t field_len = safe_read_buffer<varuint32>(&rgbPayload, &cbData);
		std::vector<char> vecrgchField;
		vecrgchField.resize(field_len);
		safe_copy_buffer(vecrgchField.data(), field_len, &rgbPayload, &cbData);
		external_kind kind = safe_read_buffer<external_kind>(&rgbPayload, &cbData);

		switch (kind)
		{
		case external_kind::Global:
		{
			value_type type = safe_read_buffer<value_type>(&rgbPayload, &cbData);
			uint8_t fMutable = safe_read_buffer<uint8_t>(&rgbPayload, &cbData);
			Verify(!fMutable);	// wasm spec says these must always be immutable
			m_vecglbls.push_back({ 0, type, !!fMutable });
			break;
		}

		case external_kind::Function:
		{
			std::string strName(vecrgchModule.begin(), vecrgchModule.end());	// TODO: string_view
			Verify(strName == "env");
			m_vecimports.push_back(0);	// for now just place hold
			m_vecimportFnNames.push_back(std::string(vecrgchField.begin(), vecrgchField.end()));
			uint32_t ifnType = safe_read_buffer<varuint32>(&rgbPayload, &cbData);
			m_vecfn_entries.push_back(ifnType);
			Verify(m_vecfn_entries.back() < m_vecfn_types.size());
			break;
		}
		default:
			Verify(false);
		}
		--cimport;
	}
}

// initializers for indirect function table
void WasmContext::load_elements(const uint8_t *rgbPayload, size_t cbData)
{
	varuint32 var32celem = safe_read_buffer<varuint32>(&rgbPayload, &cbData);
	uint32_t celem = var32celem;

	while (celem > 0)
	{
		uint32_t idx = safe_read_buffer<varuint32>(&rgbPayload, &cbData);
		Verify(idx == 0);	// MVP limitation
							// LoadExpression

		ExpressionService::Variant var;
		size_t cbExpr = ExpressionService::CbEatExpression(rgbPayload, cbData, &var);
		Verify(cbExpr <= cbData);	// This would be a bug in CbEatExpr but lets double check
		cbData -= cbExpr;
		rgbPayload += cbExpr;
		uint32_t numelem = safe_read_buffer<varuint32>(&rgbPayload, &cbData);
		Verify(var.type == value_type::i32);
		uint32_t idxStart = static_cast<uint32_t>(var.val);
		for (uint32_t ielem = 0; ielem < numelem; ++ielem)
		{
			Verify(idxStart + ielem < m_vecIndirectFnTable.size());
			m_vecIndirectFnTable[idxStart + ielem] = safe_read_buffer<varuint32>(&rgbPayload, &cbData);
		}

		--celem;
	}
	Verify(cbData == 0);
}

void WasmContext::load_data(const uint8_t *rgbPayload, size_t cbData)
{
	uint32_t csegs = safe_read_buffer<varuint32>(&rgbPayload, &cbData);

	while (csegs > 0)
	{
		uint32_t idxMem = safe_read_buffer<varuint32>(&rgbPayload, &cbData);
		Verify(idxMem == 0);	// MVP limitation

		ExpressionService::Variant varOffset;
		size_t cbExpr = ExpressionService::CbEatExpression(rgbPayload, cbData, &varOffset);
		Verify(cbExpr <= cbData);
		rgbPayload += cbExpr;
		cbData -= cbExpr;

		uint32_t offset = static_cast<uint32_t>(varOffset.val);
		uint32_t cb = safe_read_buffer<varuint32>(&rgbPayload, &cbData);

		if (offset + cb > m_vecmem.size())
		{
			m_vecmem.resize(offset + cb);
		}

		Verify((offset + cb) <= m_vecmem.size());

		safe_copy_buffer(m_vecmem.data() + offset, cb, &rgbPayload, &cbData);

		--csegs;
	}
}

void WasmContext::load_start(const uint8_t *rgbPayload, size_t cbData)
{
	m_ifnStart = safe_read_buffer<varuint32>(&rgbPayload, &cbData);
	m_fStartFn = true;
}

void WasmContext::InitializeMemory()
{
	// find out memory export
	int idxMem = -1;
	bool fFound = false;
	for (auto &exp : m_vecexports)
	{
		if (exp.strName == "memory")
		{
			Verify(idxMem == -1, "Only one memory export may be defined");
			fFound = true;
			idxMem = exp.index;
		}
	}

	if (fFound)
	{
		Verify(idxMem >= 0 && idxMem < m_vecmem_types.size(), "Invalid memory export");
		m_vecmem.resize(m_vecmem_types[idxMem].initial_size * WASM_PAGE_SIZE);
	}
}


bool WasmContext::load_section(FILE *pf)
{
	section_header header;
	std::vector<uint8_t> vecrgchName;
	std::vector<uint8_t> vecpayload;
	try
	{
		fread_struct(&header.id, pf);
		fread_struct(&header.payload_len, pf);
		if ((int)header.id == 0)
		{
			varuint32 name_len;
			auto cbStart = ftell(pf);
			fread_struct(&name_len, pf);
			vecrgchName.resize(name_len);
			fread_struct(vecrgchName.data(), pf, name_len);
			auto cbEnd = ftell(pf);
			header.payload_len -= (cbEnd - cbStart);
		}
		vecpayload.resize(header.payload_len);
		fread_struct(vecpayload.data(), pf, header.payload_len);
	}
	catch (int)
	{
		if (feof(pf))
			return false;	// valid to end the file at a section boundary
		throw;
	}

	switch (header.id)
	{
	case section_types::Custom:
		break;	//ignore custom sections
	case section_types::Type:
		load_fn_types(vecpayload.data(), vecpayload.size());
		break;
	case section_types::Import:
		load_imports(vecpayload.data(), vecpayload.size());
		break;
	case section_types::Function:
		load_fn_decls(vecpayload.data(), vecpayload.size());
		break;
	case section_types::Table:
		load_tables(vecpayload.data(), vecpayload.size());
		break;
	case section_types::Memory:
		load_memory(vecpayload.data(), vecpayload.size());
		break;
	case section_types::Global:
		load_globals(vecpayload.data(), vecpayload.size());
		break;
	case section_types::Export:
		load_exports(vecpayload.data(), vecpayload.size());
		InitializeMemory();
		break;
	case section_types::Element:
		load_elements(vecpayload.data(), vecpayload.size());
		break;
	case section_types::Code:
		load_code(vecpayload.data(), vecpayload.size());
		break;
	case section_types::Data:
		load_data(vecpayload.data(), vecpayload.size());
		break;
	case section_types::Start:
		load_start(vecpayload.data(), vecpayload.size());
		break;

	default:
		throw std::string("unknown section");
	}
	return true;
}


ExpressionService::Variant WasmContext::CallFunction(const char *szName, ExpressionService::Variant *rgargs, uint32_t cargs)
{
	bool fExecuted = false;
	ExpressionService::Variant varRet;
	for (size_t iexport = 0; iexport < m_vecexports.size(); ++iexport)
	{
		if (m_vecexports[iexport].strName == szName)
		{
			uint32_t ifn = m_vecexports[iexport].index;
			varRet = m_spjitwriter->ExternCallFn(ifn, m_vecmem.data(), rgargs, cargs);
			fExecuted = true;
			break;
		}
	}
	Verify(fExecuted);
	return varRet;
}

void WasmContext::LinkImports()
{
	for (size_t iimport = 0; iimport < m_vecimports.size(); ++iimport)
	{
		int ibuiltin = IBuiltinFromName(m_vecimportFnNames.at(iimport));
		//Verify(FEqualProto(BuiltinMap[ibuiltin], *g_vecfn_types.at(g_vecfn_entries.at(iimport))));
		m_vecimports[iimport] = ibuiltin;
	}
}

void WasmContext::LoadModule(FILE *pf)
{
	wasm_file_header header;
	fread_struct(&header, pf);

	Verify(header.magic == 0x6d736100U, "Invalid wasm magic value");
	Verify(header.version == 1, "Unknown version");
	
	while (load_section(pf));
	Verify(feof(pf));

	m_spjitwriter = std::unique_ptr<JitWriter>(new JitWriter(this, m_vecfn_entries.size(), m_vecglbls.size()));
	LinkImports();

	for (auto &itr : m_vecfn_entries)
	{
		itr = ITypeCanonicalFromIType(itr);
	}

	if (m_fStartFn)
	{
		m_spjitwriter->ExternCallFn(m_ifnStart, m_vecmem.data(), nullptr, 0);
	}
}

uint32_t WasmContext::ITypeCanonicalFromIType(uint32_t idx)
{
	Verify(idx < m_vecfn_types.size());
	if (idx == 0)
		return idx;
	
	auto itrType = m_vecfn_types.begin() + idx;
	do
	{
		itrType = itrType - 1;
		if (**itrType == *m_vecfn_types[idx])
		{
			idx = numeric_cast<uint32_t>(itrType - m_vecfn_types.begin());
		}
	} while (itrType != m_vecfn_types.begin());
	return idx;
}
