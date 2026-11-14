#include "Cafe/OS/common/OSCommon.h"
#include "util/MemMapper/MemMapper.h"
#include "virt_range.h"

#define OS_MAP_READ_ONLY		(1)
#define OS_MAP_READ_WRITE		(2)

#define VIRT_RANGE_ADDR			(0xA0000000) // todo: Process specific. For the main ram pid this overlaps with the overlay arena?
#define VIRT_RANGE_SIZE			(0x40000000)

#define PHYS_RANGE_ADDR			(0x10000000) // todo: Process specific
#define PHYS_RANGE_SIZE			(0x40000000)

namespace coreinit
{
	virt_range_t s_virtRange;

	MPTR _OSAllocVirtAddr(uint32 size, uint32 alignment)
	{
		MPTR addr = virt_range_alloc(&s_virtRange, size, alignment);
		if (!addr)
			cemuLog_log(LogType::APIErrors, "_OSAllocVirtAddr(): Unable to allocate memory\n");
		return addr;
	}

	bool _OSFreeVirtAddr(MPTR virtAddr)
	{
		return virt_range_free(&s_virtRange, virtAddr);
	}

	void OSGetAvailPhysAddrRange(uint32be* physRangeStart, uint32be* physRangeSize)
	{
		*physRangeStart = PHYS_RANGE_ADDR;
		*physRangeSize = PHYS_RANGE_SIZE;
	}

	void* OSAllocVirtAddr(MEMPTR<void> address, uint32 size, uint32 align)
	{
		if (align == 0)
			align = 1;
		if (align != 0 && align != 1)
			assert_dbg();
		cemu_assert_debug(address == nullptr); // todo - support for allocation with fixed address

		address = _OSAllocVirtAddr(size, align);
		debug_printf("OSAllocVirtAddr(): Allocated virtual memory at 0x%08x\n", address.GetMPTR());
		return MEMPTR<void>(address);
	}

	uint32 OSFreeVirtAddr(MEMPTR<void> address)
	{
		bool r = _OSFreeVirtAddr(address.GetMPTR());
		if(!r)
			cemuLog_log(LogType::APIErrors, "OSFreeVirtAddr: Could not find allocation with address 0x{:08x}\n", address.GetMPTR());
		return r ? 1 : 0;
	}

	uint32 OSMapMemory(MPTR virtualAddress, MPTR physicalAddress, uint32 size, uint32 mode)
	{
		if (virtualAddress < VIRT_RANGE_ADDR || virtualAddress >= (VIRT_RANGE_ADDR + VIRT_RANGE_SIZE))
		{
			cemuLog_log(LogType::APIErrors, "OSMapMemory: Virtual address out of bounds\n");
			return 0;
		}

		uint8* virtualPtr = memory_getPointerFromVirtualOffset(virtualAddress);

		MemMapper::PAGE_PERMISSION pageProtect = MemMapper::PAGE_PERMISSION::P_NONE;
		if (mode == OS_MAP_READ_ONLY)
			pageProtect = MemMapper::PAGE_PERMISSION::P_READ;
		else if (mode == OS_MAP_READ_WRITE)
			pageProtect = MemMapper::PAGE_PERMISSION::P_RW;
		else
			cemu_assert_unimplemented();
		void* allocationResult = MemMapper::AllocateMemory(virtualPtr, size, pageProtect, true);
		if (!allocationResult)
		{
			cemuLog_log(LogType::Force, "OSMapMemory failed");
			return 0;
		}
		return 1;
	}

	uint32 OSUnmapMemory(MPTR virtualAddress, uint32 size)
	{
		if (virtualAddress < VIRT_RANGE_ADDR || virtualAddress >= (VIRT_RANGE_ADDR + VIRT_RANGE_SIZE))
			cemu_assert_suspicious();

		cemu_assert((size % MemMapper::GetPageSize()) == 0);

		uint8* virtualPtr = memory_getPointerFromVirtualOffset(virtualAddress);

		MemMapper::FreeMemory(virtualPtr, size, true);
		return 1;
	}

	void InitializeMemoryMapping()
	{
		virt_range_init(&s_virtRange, VIRT_RANGE_ADDR, VIRT_RANGE_SIZE,
			std::max<uint32>((uint32)MemMapper::GetPageSize(), Espresso::MEM_PAGE_SIZE));
		cafeExportRegister("coreinit", OSGetAvailPhysAddrRange, LogType::CoreinitMemoryMapping);
		cafeExportRegister("coreinit", OSAllocVirtAddr, LogType::CoreinitMemoryMapping);
		cafeExportRegister("coreinit", OSFreeVirtAddr, LogType::CoreinitMemoryMapping);
		cafeExportRegister("coreinit", OSMapMemory, LogType::CoreinitMemoryMapping);
		cafeExportRegister("coreinit", OSUnmapMemory, LogType::CoreinitMemoryMapping);
	}
}