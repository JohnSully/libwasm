#pragma once
#include "wasm_types.h"
#include "ExpressionService.h"

class WasmContext
{
	friend class JitWriter;

public:
	WasmContext();
	~WasmContext();

	ExpressionService::Variant CallFunction(const char *szName, ExpressionService::Variant *rgargs = nullptr, uint32_t cargs = 0);
	void LoadModule(FILE *pfModule);

protected:
	// File Load Helpers
	void load_fn_type(const uint8_t **prgbPayload, size_t *pcbData);
	void load_fn_types(const uint8_t *rgbPayload, size_t cbData);
	void load_fn_decls(const uint8_t *rgbPayload, size_t cbData);
	void load_tables(const uint8_t *rgbPayload, size_t cbData);
	void load_memory(const uint8_t *rgbPayload, size_t cbData);
	void load_globals(const uint8_t *rgbPayload, size_t cbData);
	void load_exports(const uint8_t *rgbPayload, size_t cbData);
	void load_code(const uint8_t *rgbPayload, size_t cbData);
	void load_imports(const uint8_t *rgbPayload, size_t cbData);
	void load_elements(const uint8_t *rgbPayload, size_t cbData);
	void load_data(const uint8_t *rgbPayload, size_t cbData);
	void load_start(const uint8_t *rgbPayload, size_t cbData);
	bool load_section(FILE *pf);

	void InitializeMemory();
	void LinkImports();

	uint32_t ITypeCanonicalFromIType(uint32_t idx);

	struct GlobalVar
	{
		uint64_t val;
		value_type type;
		bool fMutable;
	};
	std::vector<GlobalVar> m_vecglbls;
	std::vector<FunctionTypeEntry::unique_pfne_ptr> m_vecfn_types;
	std::vector<uint32_t> m_vecfn_entries;
	std::vector<table_type> m_vectbl;
	std::vector<resizable_limits> m_vecmem_types;
	std::vector<int> m_vecimports;
	std::vector<uint32_t> m_vecIndirectFnTable;
	std::vector<std::string> m_vecimportFnNames;
	std::vector<export_entry> m_vecexports;
	std::vector<FunctionCodeEntry::unique_pfne_ptr> m_vecfn_code;
	std::vector<uint8_t> m_vecmem;

	bool m_fStartFn = false;
	uint32_t m_ifnStart = 0;

	std::unique_ptr<class JitWriter> m_spjitwriter;
};
