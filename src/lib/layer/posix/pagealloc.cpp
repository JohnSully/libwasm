#include <cstdlib>
#include <inttypes.h>
#include <memory>
#include "../layer.h"
#include <unistd.h>
#include <sys/mman.h>
#include <assert.h>
#include <new>

namespace layer
{

class AllocatedPageBlockUnix : public AllocatedPageBlock
{
public:
	AllocatedPageBlockUnix(void *pv, size_t cb)
	{
		assert(pv != nullptr && cb > 0);
		m_pv = pv;
		m_cb = cb;
	}
	~AllocatedPageBlockUnix()
	{
		munmap(m_pv, m_cb);
	}

};

std::unique_ptr<AllocatedPageBlock> ReservePages(const void *pvBaseRequested, size_t cb)
{
	void *pv = mmap((void*)pvBaseRequested, cb, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (pv == MAP_FAILED)
		throw std::bad_alloc();
	return std::unique_ptr<AllocatedPageBlock>(new AllocatedPageBlockUnix(pv, cb));
}

void ProtectRange(AllocatedPageBlock &block, void *pvAddrStart, size_t cbRange, PAGE_PROTECTION prot)
{
	int unixprot = PROT_NONE;
	switch (prot)
	{
	case PAGE_PROTECTION::ReadOnly:
		unixprot = PROT_READ;
		break;
	case PAGE_PROTECTION::ReadWrite:
		unixprot = PROT_READ | PROT_WRITE;
		break;
	case PAGE_PROTECTION::ReadExecute:
		unixprot = PROT_READ | PROT_EXEC;
		break;
	case PAGE_PROTECTION::Unallocated:
		unixprot = PROT_NONE;
		break;
	}
	if (pvAddrStart == nullptr)
		pvAddrStart = block.PvBaseAddr();
	int ret = mprotect(pvAddrStart, cbRange, unixprot);
	if (ret != 0)
		throw std::bad_alloc();
}
};
