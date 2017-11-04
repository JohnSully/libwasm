#include <cstdlib>
#include <inttypes.h>
#include <memory>
#include "../layer.h"
#include <Windows.h>
#include <assert.h>
#include <new>

namespace layer
{

	class AllocatedPageBlockWindows : public AllocatedPageBlock
	{
	public:
		AllocatedPageBlockWindows(void *pv, size_t cb)
		{
			assert(pv != nullptr && cb > 0);
			m_pv = pv;
			m_cb = cb;
		}
		~AllocatedPageBlockWindows()
		{
			VirtualFree(m_pv, 0, MEM_RELEASE);
		}

	};

	std::unique_ptr<AllocatedPageBlock> ReservePages(const void *pvBaseRequested, size_t cb)
	{
		void *pv = VirtualAlloc((void*)pvBaseRequested, cb, MEM_RESERVE | MEM_COMMIT, PAGE_NOACCESS);
		if (pv == nullptr)
			throw std::bad_alloc();
		return std::make_unique<AllocatedPageBlockWindows>(pv, cb);
	}

	void ProtectRange(AllocatedPageBlock &block, void *pvAddrStart, size_t cbRange, PAGE_PROTECTION prot)
	{
		int winprot = PAGE_NOACCESS;
		switch (prot)
		{
		case PAGE_PROTECTION::ReadOnly:
			winprot = PAGE_READONLY;
			break;
		case PAGE_PROTECTION::ReadWrite:
			winprot = PAGE_READWRITE;
			break;
		case PAGE_PROTECTION::ReadExecute:
			winprot = PAGE_EXECUTE_READ;
			break;
		case PAGE_PROTECTION::Unallocated:
			winprot = PAGE_NOACCESS;
			break;
		}
		if (pvAddrStart == nullptr)
			pvAddrStart = block.PvBaseAddr();
		DWORD dwT;
		bool ret = VirtualProtect(pvAddrStart, cbRange, winprot, &dwT);
		if (!ret)
			throw std::bad_alloc();
	}
};
