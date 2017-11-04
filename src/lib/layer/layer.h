#pragma once

namespace layer
{

/* Platform Abstraction Layer defines */
class AllocatedPageBlock
{
public:
	virtual ~AllocatedPageBlock() {}
	AllocatedPageBlock(const AllocatedPageBlock &other) = delete;
	AllocatedPageBlock(AllocatedPageBlock &&other) = delete;

	void *PvBaseAddr() const { return m_pv; }
	size_t Cb() const { return m_cb; }
protected:
	AllocatedPageBlock() = default;

	void *m_pv = nullptr;
	size_t m_cb = 0;
};

enum class PAGE_PROTECTION
{
	ReadOnly,
	ReadWrite,
	ReadExecute,
	Unallocated,
};

// ReservePages reserves a range but does not commit it
std::unique_ptr<AllocatedPageBlock> ReservePages(const void *pvBaseRequested, size_t cb);

void ProtectRange(AllocatedPageBlock &block, void *pvAddrStart, size_t cbRange, PAGE_PROTECTION prot);
};
